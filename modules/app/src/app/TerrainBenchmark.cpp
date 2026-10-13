/**
 * @file TerrainBenchmark.cpp
 * @author Jackey Cen
 * @brief Drives repeatable terrain routes and records engine benchmark JSON.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBenchmark.h"
#include "TerrainRuntime.h"

#include <app/App.h>
#include <core/ecs/EngineMode.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/RenderSystem.h>
#include <serial/JsonFile.h>
#include <terrain/TerrainAssetPath.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <stdexcept>
#include <thread>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        glm::dvec3 Vector(const Json &value)
        {
            if (!value.is_array() || value.size() != 3)
                throw std::runtime_error("expected a vector with three coordinates");
            glm::dvec3 result{value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
            if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
                throw std::runtime_error("coordinates must be finite");
            return result;
        }

        WorldPosition Position(const glm::dvec3 &meters)
        {
            const auto position = WorldPosition::FromDoubleMeters(meters);
            if (!position)
                throw std::runtime_error("benchmark coordinate cannot be represented");
            return *position;
        }
    }

    std::unique_ptr<TerrainBenchmark> TerrainBenchmark::Load(const std::filesystem::path &path)
    {
        const auto json = ReadJsonFile(path);
        if (!json)
        {
            MIR_LOG_ERROR("Terrain benchmark configuration unavailable: {}", path.string());
            return nullptr;
        }
        try
        {
            auto benchmark = std::make_unique<TerrainBenchmark>();
            benchmark->mPlan = *json;
            benchmark->mConfigPath = std::filesystem::absolute(path);
            const Json &plan = benchmark->mPlan;
            if (plan.at("schemaVersion").get<int>() != 1)
                throw std::runtime_error("unsupported benchmark schemaVersion");
            const double dt = plan.at("fixedDeltaSeconds").get<double>();
            if (!std::isfinite(dt) || dt <= 0.0)
                throw std::runtime_error("fixedDeltaSeconds must be positive");
            (void)plan.at("pace").get<bool>();
            (void)plan.at("rayTracing").get<bool>();
            (void)plan.at("validation").get<bool>();
            (void)plan.at("cacheState").get<std::string>();
            (void)plan.at("hardware");
            const auto &routes = plan.at("routes");
            if (!routes.is_array() || routes.empty())
                throw std::runtime_error("routes must be a nonempty array");
            for (const Json &route : routes)
            {
                (void)route.at("name").get<std::string>();
                const int64_t frames = route.at("frames").get<int64_t>();
                const int64_t views = route.at("viewports").get<int64_t>();
                const int64_t interests = route.at("interests").get<int64_t>();
                const int64_t failures = route.at("failureReads").get<int64_t>();
                const double fov = route.at("fovYDegrees").get<double>();
                const double spacing = route.at("querySpacingMeters").get<double>();
                const double yaw = route.at("yawDegreesPerSecond").get<double>();
                const double interestHeight = route.at("interestHeightMeters").get<double>();
                if (!std::isfinite(interestHeight))
                    throw std::runtime_error("interest height must be finite");
                if (!route.at("landingClearanceMeters").is_null() &&
                    (!std::isfinite(route.at("landingClearanceMeters").get<double>()) ||
                     route.at("landingClearanceMeters").get<double>() <= 0.0))
                    throw std::runtime_error("landing clearance must be positive or null");
                const glm::dvec3 from = Vector(route.at("from")), to = Vector(route.at("to"));
                const glm::dvec3 target = Vector(route.at("target"));
                (void)Position(from);
                (void)Position(to);
                if (frames <= 0 || views < 1 || views > 2 || interests < 0 || failures < 0 ||
                    !std::isfinite(fov) || fov <= 0.0 || fov >= 180.0 || !std::isfinite(spacing) || spacing <= 0.0 ||
                    !std::isfinite(yaw) || glm::length(target - from) == 0.0 || glm::length(target - to) == 0.0)
                    throw std::runtime_error("invalid route frame count, viewports, interests, faults or camera");
                const std::string action = route.at("action").get<std::string>();
                if (action != "none" && action != "reload" && action != "stop" && action != "play" &&
                    action != "publish" && action != "play_after_switch")
                    throw std::runtime_error("unknown route action");
                if (action == "publish")
                    (void)route.at("candidate").get<std::string>();
                benchmark->mTotalFrames += uint64_t(frames);
            }
            benchmark->mOutputPath = benchmark->mConfigPath.parent_path() / plan.at("output").get<std::string>();
            return benchmark;
        }
        catch (const std::exception &error)
        {
            MIR_LOG_ERROR("Terrain benchmark '{}': {}", path.string(), error.what());
            return nullptr;
        }
    }

    void TerrainBenchmark::Configure(TerrainRuntime &terrain, const std::filesystem::path &releases)
    {
        terrain.ConfigureTelemetry(mTelemetry, [releases, telemetry = mTelemetry, faults = mFaults](AssetId asset,
                                                                                                  TerrainReleaseId id)
        {
            auto source = std::make_shared<TerrainProfiledSource>(
                std::make_shared<TerrainFileSource>(TerrainReleaseDir(releases, asset, id)), telemetry, faults,
                TerrainReleaseInfo{.asset = asset, .release = id});
            return TerrainReleaseHandle::Open(std::move(source));
        });
    }

    bool TerrainBenchmark::Start(App &app)
    {
        if (app.Project().mDir != "games/TerrainCheck" || app.GetWorld().Resource<TerrainWorld>().Empty())
        {
            MIR_LOG_ERROR("Terrain benchmark requires TerrainCheck with loaded terrain");
            return false;
        }
        const bool gpuTiming = app.Renderer().EnableFrameProfiling();
        const auto &info = app.Renderer().ProfileInfo();
        mOutput = {{"schemaVersion", 1}, {"plan", mPlan}, {"configPath", mConfigPath.string()},
                    {"gpu", {{"device", info.device}, {"driverVersion", info.driverVersion},
                              {"timestampPeriodNanoseconds", info.timestampPeriodNanoseconds},
                              {"timestampValidBits", info.timestampValidBits}, {"timingAvailable", gpuTiming}}},
#ifdef NDEBUG
                    {"buildConfiguration", "Release"},
#else
                    {"buildConfiguration", "Debug"},
#endif
                    {"frames", Json::array()}, {"samples", Json::array()}, {"gpuFrames", Json::array()}};
        mOutput["settings"]["game"] = ReadJsonFile(app.Project().mRootDir / kTerrainStreamingSettingsFileName).value_or(Json{});
        const auto waits = app.Renderer().MeshStreaming();
        mOutput["initialWaitCounts"] = {{"device", waits.deviceWaits}, {"queue", waits.queueWaits}};
        mOutput["settings"]["local"] = ReadJsonFile(ExecutableDir() / kTerrainStreamingLocalSettingsFileName).value_or(Json{});
        mOutput["maps"] = Json::array();
        for (const auto &instance : app.GetWorld().Resource<TerrainWorld>().Instances())
        {
            const auto &index = instance.release->Handle().Index();
            const auto translation = instance.placement.translation.Meters();
            mOutput["maps"].push_back({{"recipe", instance.recipePath}, {"asset", TerrainIdHex(index.release.asset.value)},
                {"release", TerrainIdHex(index.release.release.value)}, {"sourceFingerprint", TerrainIdHex(index.release.source.value)},
                {"payloadRecords", index.tiles.size()}, {"regions", index.regions.size()},
                {"regionLevels", index.regionGrid ? index.regionGrid->Levels() : 0},
                {"regionTiles", index.regionGrid ? index.regionGrid->RegionTiles() : 0},
                {"placement", {translation.x, translation.y, translation.z}}});
        }
        app.GetWorld().Resource<RenderSettings>().rayTracing = mPlan.at("rayTracing").get<bool>();
        if (!app.GetWorld().TryResource<TerrainCollisionSources>())
            app.GetWorld().EmplaceResource<TerrainCollisionSources>();
        mStart = std::chrono::steady_clock::now();
        mStarted = true;
        return true;
    }

    float TerrainBenchmark::DeltaSeconds() const { return mPlan.at("fixedDeltaSeconds").get<float>(); }

    void TerrainBenchmark::Pace()
    {
        if (mPlan.at("pace").get<bool>())
            std::this_thread::sleep_until(mStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(double(mFrame) * DeltaSeconds())));
    }

    void TerrainBenchmark::BeginFrame(App &app, uint64_t frame)
    {
        mFrame = frame;
        auto &world = app.GetWorld();
        mRouteChanged = frame == 0 || frame - mRouteStart >= mPlan.at("routes").at(mRoute).at("frames").get<uint64_t>();
        if (mRouteChanged)
        {
            if (frame != 0)
            {
                ++mRoute;
                mRouteStart = frame;
            }
            const Json &route = mPlan.at("routes").at(mRoute);
            mFaults->remaining = route.at("failureReads").get<uint64_t>();
            const std::string action = route.at("action").get<std::string>();
            if (action == "stop" || action == "reload")
                app.StopScene();
            if (action == "reload")
            {
                world.Resource<TerrainStreamer>().EvictPayloads();
                if (!app.LoadScene())
                    mFailed = true;
                app.PlayScene();
            }
            if (action == "play")
                app.PlayScene();
            if (action == "publish")
            {
                const std::filesystem::path candidate = mConfigPath.parent_path() / route.at("candidate").get<std::string>();
                std::vector<std::string> args{"publish", "--candidate", candidate.string(), "--releases",
                    TerrainReleasesRoot(app.Project().mRootDir).string(), "--report", mOutputPath.string() + ".publish.json"};
                if (!mPublisher.Start(ExecutableDir() / "TerrainTool.exe", args))
                    mFailed = true;
            }
            MIR_LOG_INFO("Terrain benchmark route {}: {}", mRoute, route.at("name").get<std::string>());
            auto &sources = world.Resource<TerrainCollisionSources>();
            for (const auto id : mInterests)
                sources.Remove(id);
            mInterests.clear();
            for (size_t i = 0; i < route.at("interests").get<size_t>(); ++i)
                mInterests.push_back(sources.Add({}));
        }
        if (mPublisher.Running())
            (void)mPublisher.Poll();
        else if (const auto exit = mPublisher.Poll(); exit && *exit != 0)
            mFailed = true;
        const Json &route = mPlan.at("routes").at(mRoute);
        if (route.at("action") == "play_after_switch" && IsEditing(world))
        {
            const auto &maps = world.Resource<TerrainWorld>();
            bool switched = false;
            for (const auto &asset : maps.Assets())
                if (asset.release)
                    for (const auto &initial : mOutput.at("maps"))
                        switched = switched || (initial.at("asset") == TerrainIdHex(asset.asset.value) &&
                            initial.at("release") != TerrainIdHex(asset.release->Info().release.value));
            if (switched)
                app.PlayScene();
        }
    }

    void TerrainBenchmark::ApplyCameras(App &app, RenderSystem &render)
    {
        const Json &route = mPlan.at("routes").at(mRoute);
        const uint64_t frameCount = route.at("frames").get<uint64_t>();
        const double fraction = frameCount > 1 ? double(mFrame - mRouteStart) / double(frameCount - 1) : 0.0;
        const glm::dvec3 from = Vector(route.at("from")), to = Vector(route.at("to"));
        glm::dvec3 eye = glm::mix(from, to, fraction);
        if (!route.at("landingClearanceMeters").is_null())
        {
            const auto height = app.GetWorld().Resource<TerrainWorld>().HeightAt(Position(eye),
                route.at("querySpacingMeters").get<double>());
            if (height.HasHeight() && height.worldPoint)
                eye.y = glm::mix(eye.y, double(height.worldPoint->Meters().y) +
                    route.at("landingClearanceMeters").get<double>(), fraction * fraction * fraction);
        }
        mEye = Position(eye);
        glm::vec3 direction = glm::normalize(glm::vec3(Vector(route.at("target")) - eye));
        const double yaw = route.at("yawDegreesPerSecond").get<double>() * double(mFrame - mRouteStart) * DeltaSeconds();
        direction = glm::angleAxis(glm::radians(float(yaw)), glm::vec3(0, 1, 0)) * direction;
        const size_t views = route.at("viewports").get<size_t>();
        const VkExtent2D extent = app.Renderer().SwapchainExtent();
        if (views > mViewports)
            render.AddViewport({});
        else if (views < mViewports)
            render.RemoveViewport(1);
        mViewports = views;
        for (size_t i = 0; i < views; ++i)
        {
            Camera lens;
            lens.mFovYDegrees = route.at("fovYDegrees").get<float>() / float(i + 1);
            lens.mFarZ = 0.0f;
            const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            const glm::mat3 rotation = glm::transpose(glm::mat3(glm::lookAt(glm::vec3(0), direction, up)));
            render.SetViewportRect(i, views == 1 ? VkRect2D{} : VkRect2D{
                {int32_t(i * (extent.width / 2)), 0}, {extent.width / 2, extent.height}});
            render.SetExternalCamera(i, WorldPose{Position(eye), rotation}, lens);
        }
        auto &sources = app.GetWorld().Resource<TerrainCollisionSources>();
        const double duration = double(frameCount - 1) * DeltaSeconds();
        const glm::dvec3 velocity = duration > 0.0 ? (to - from) / duration : glm::dvec3(0);
        const auto &settings = app.GetWorld().Resource<TerrainStreamingSettings>();
        for (size_t i = 0; i < mInterests.size(); ++i)
        {
            glm::dvec3 at = eye + glm::dvec3(double(i) * 256.0, 0, 0);
            at.y = route.at("interestHeightMeters").get<double>();
            TerrainCollisionSource source;
            source.position = Position(at);
            source.velocity = {velocity.x, 0.0, velocity.z};
            source.radius = 2.0;
            source.tolerance = settings.collision.bodyToleranceMeters;
            source.floor = -20.0;
            sources.Set(mInterests[i], source);
        }
    }

    bool TerrainBenchmark::EndFrame(App &app, TerrainRuntime &terrain, uint64_t frame,
                                    double cpuSeconds, double intervalSeconds)
    {
        const auto profileStart = std::chrono::steady_clock::now();
        Json row = terrain.ProfileSnapshot();
        row["frame"] = frame;
        row["route"] = mPlan.at("routes").at(mRoute).at("name");
        row["routeFrame"] = frame - mRouteStart;
        row["boundary"] = mRouteChanged;
        row["cpuFrameMs"] = cpuSeconds * 1000.0;
        row["intervalMs"] = intervalSeconds * 1000.0;
        row["gpuSerial"] = app.Renderer().LastProfileSerial();
        row["processBytes"] = CurrentProcessMemoryUsageBytes();
        row["deviceAllocatedBytes"] = app.Renderer().GetGpuMemoryUsage().mUsageBytes;
        row["mainThreadRangeReads"] = mFaults->mainThreadRanges.load();
        row["injectedFailures"] = mFaults->injected.load();
        const auto uploads = app.Renderer().MeshStreaming();
        row["upload"] = {{"pendingBytes", uploads.pendingBytes}, {"uploadingBytes", uploads.uploadingBytes},
            {"retiringBytes", uploads.retiringBytes}, {"stagingUsedBytes", uploads.stagingUsedBytes},
            {"stagingCapacityBytes", uploads.desc.stagingBytes}, {"uploadedBytes", uploads.uploadedBytes},
            {"frameBytes", uploads.lastFrameBytes}, {"blasBuilt", uploads.blasBuilt},
            {"deviceWaits", uploads.deviceWaits}, {"queueWaits", uploads.queueWaits}, {"skippedDraws", uploads.skippedDraws}};
        row["capabilityProbes"] = Json::array();
        const Json &route = mPlan.at("routes").at(mRoute);
        const auto start = std::chrono::steady_clock::now();
        const auto result = app.GetWorld().Resource<TerrainWorld>().HeightAt(mEye, route.at("querySpacingMeters").get<double>());
        mTelemetry->Record("height_query", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        row["heightStatus"] = int(result.status);
        row["heightHasResult"] = result.HasHeight();
        const auto eye = mEye.Meters();
        row["cameraPosition"] = {eye.x, eye.y, eye.z};
        row["heightWorldY"] = result.HasHeight() && result.worldPoint
            ? Json(result.worldPoint->Meters().y) : Json(nullptr);
        row["heightRelease"] = TerrainIdHex(result.release.release.value);
        for (const auto id : mInterests)
        {
            const auto status = app.GetWorld().Resource<TerrainCollisionSources>().Status(id);
            row["capabilityProbes"].push_back({{"source", id}, {"state", TerrainCollisionStateName(status.state)},
                {"needed", status.needed}, {"ready", status.ready}, {"pending", status.pending},
                {"failed", status.failed}, {"contactError", status.contactError}});
        }
        mOutput["frames"].push_back(std::move(row));
        for (const auto &sample : mTelemetry->TakeSamples())
            mOutput["samples"].push_back({{"metric", sample.metric}, {"key", sample.key},
                {"asset", TerrainIdHex(sample.release.asset.value)}, {"release", TerrainIdHex(sample.release.release.value)},
                {"atSeconds", sample.atSeconds}, {"ms", sample.seconds * 1000.0}, {"success", sample.success},
                {"route", route.at("name")}});
        for (const auto &sample : app.Renderer().TakeFrameTimings())
            mOutput["gpuFrames"].push_back({{"serial", sample.serial}, {"gpuMs", sample.gpuSeconds * 1000.0},
                {"uploadMs", sample.uploadSeconds * 1000.0}, {"primaryDrawCalls", sample.primaryDrawCalls},
                {"rayTracing", sample.rayTracing}});
        for (const auto seconds : app.Renderer().TakeMeshRetirementTimings())
            mOutput["samples"].push_back({{"metric", "gpu_retirement"}, {"ms", seconds * 1000.0},
                                          {"success", true}, {"route", route.at("name")}});
        const double overhead = std::chrono::duration<double>(std::chrono::steady_clock::now() - profileStart).count();
        mOutput["frames"].back()["telemetryMs"] = overhead * 1000.0;
        mOutput["frames"].back()["cpuFrameMs"] = (cpuSeconds + overhead) * 1000.0;
        ++mFrame;
        return frame + 1 >= mTotalFrames;
    }

    bool TerrainBenchmark::Save(bool completed)
    {
        mOutput["completed"] = completed && !mFailed;
        mOutput["pendingRequests"] = mTelemetry->Pending();
        mOutput["publisherRunning"] = mPublisher.Running();
        std::error_code error;
        std::filesystem::create_directories(mOutputPath.parent_path(), error);
        if (error || !WriteJsonFile(mOutputPath, mOutput))
            return false;
        MIR_LOG_INFO("Terrain benchmark saved {} frames to {}", mOutput["frames"].size(), mOutputPath.string());
        if (mFailed)
            MIR_LOG_ERROR("Terrain benchmark route action failed; see the trace");
        return completed && !mFailed;
    }
}
