/**
 * @file TerrainRuntime.cpp
 * @author Jackey Cen
 * @brief The scene's terrain meshes, colliders and TerrainWorld, swapped together in Edit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRuntime.h"

#include <core/ecs/World.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <renderer/RenderSettings.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainCollision.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainStreamingSettings.h>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        bool SamePlacement(const TerrainPlacement &a, const TerrainPlacement &b)
        {
            return a.asset == b.asset && a.translation.Meters() == b.translation.Meters();
        }

        double MiB(uint64_t bytes)
        {
            return double(bytes) / (1024.0 * 1024.0);
        }

        uint64_t ReleaseFootprint(const TerrainIndex &index)
        {
            uint64_t bytes = 0;
            for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
                for (const TerrainTileRecord &record : *records)
                    bytes += TerrainPayloadFootprint(record);
            return bytes;
        }
    }

    TerrainRuntime::~TerrainRuntime()
    {
        if (!mWorld)
            return;
        if (ViewDrawSources *sources = mWorld->TryResource<ViewDrawSources>())
            std::erase(sources->sources, static_cast<IViewDrawSource *>(this));
    }

    void TerrainRuntime::Bind(World &world, VulkanRenderer &renderer, const GameProject &project)
    {
        mWorld = &world;
        mRenderer = &renderer;
        mAssetIdRoot = project.mAssetIdRoot;
        mReleasesRoot = TerrainReleasesRoot(project.mRootDir);
        ViewDrawSources *sources = world.TryResource<ViewDrawSources>();
        if (!sources)
            sources = &world.EmplaceResource<ViewDrawSources>();
        sources->sources.push_back(this);

        const TerrainStreamingSettingsLoad load =
            LoadTerrainStreamingSettings(project.mRootDir / kTerrainStreamingSettingsFileName,
                                         ExecutableDir() / kTerrainStreamingLocalSettingsFileName);
        if (!load.settings)
        {
            mSettingsProblem.clear();
            for (const std::string &problem : load.problems)
                mSettingsProblem += (mSettingsProblem.empty() ? "" : "; ") + problem;
            // Without either file the game may simply have no terrain; Apply reports it if it does.
            if (load.gameFileFound || load.localFileFound)
            {
                MIR_LOG_ERROR("Terrain streaming is not configured, so no terrain loads: {}", mSettingsProblem);
                mSettingsReported = true;
            }
            return;
        }

        const TerrainStreamingSettings &settings = *load.settings;
        world.EmplaceResource<TerrainStreamingSettings>(settings);
        mPool = std::make_shared<TerrainWorkerPool>(settings.io.workerThreads);
        world.EmplaceResource<TerrainStreamer>(settings, mPool);
        MIR_LOG_INFO("Terrain streaming: {:.0f} MiB CPU budget, {} read workers, {} residency{}",
                     MiB(settings.cpu.budgetBytes), settings.io.workerThreads,
                     TerrainResidencyModeName(*settings.residency.mode),
                     load.localFileFound ? " (machine-local overrides applied)" : "");
        MIR_LOG_INFO("Terrain views: refine above {} px, merge below {} px, prefetch to {} px within {:.1f} MiB",
                     settings.view.refineErrorPixels, settings.view.mergeErrorPixels,
                     settings.view.prefetchErrorPixels, MiB(*settings.view.prefetchBudgetBytes));

        const TerrainStreamingSettings::Gpu &gpu = settings.gpu;
        if (!renderer.ConfigureMeshStreaming({.stagingBytes = gpu.stagingBytes, .frameBytes = gpu.uploadBytesPerFrame}))
        {
            MIR_LOG_ERROR("Terrain meshes cannot stream to the GPU, so no terrain is drawn");
            return;
        }
        mUploader = std::make_unique<TerrainGpuUploader>(renderer);
        mMeshes = std::make_unique<TerrainMeshResidency>(gpu, mPool, *mUploader);
        MIR_LOG_INFO("Terrain GPU meshes: {:.0f} MiB budget, {:.1f} MiB uploaded a frame through a {:.1f} MiB staging "
                     "ring, kept {} s after their last want or draw",
                     MiB(gpu.budgetBytes), MiB(gpu.uploadBytesPerFrame), MiB(gpu.stagingBytes), gpu.meshRetainSeconds);
    }

    TerrainStreamer *TerrainRuntime::Streamer() const
    {
        return mWorld ? mWorld->TryResource<TerrainStreamer>() : nullptr;
    }

    double TerrainRuntime::Now() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }

    void TerrainRuntime::Update()
    {
        if (TerrainStreamer *streamer = Streamer())
            streamer->Update();
        if (mMeshes)
            mMeshes->Install();
    }

    void TerrainRuntime::Load()
    {
        if (!Bound())
            return;
        mReported.clear();
        mSincePoll = 0.0f;
        mPendingSpec = CollectTerrainScene(*mWorld, mAssetIdRoot);
        mPendingAge = 0.0f;
        Apply(mPendingSpec);
        TagColliders(mPendingSpec);
    }

    void TerrainRuntime::Refresh(float dt, bool force)
    {
        if (!Bound())
            return;

        TerrainSceneSpec spec = CollectTerrainScene(*mWorld, mAssetIdRoot);
        if (spec == mPendingSpec)
            mPendingAge += dt;
        else
        {
            mPendingSpec = spec;
            mPendingAge = 0.0f;
        }
        const bool edited = spec != mSpec && (force || mPendingAge >= kTerrainSettleSeconds);

        mSincePoll += dt;
        const bool poll = force || mSincePoll >= kTerrainPollSeconds;
        if (poll)
            mSincePoll = 0.0f;
        const bool moved = poll && TerrainCurrentMoved(mWorld->Resource<TerrainWorld>(), mReleasesRoot);

        if (!edited && !moved)
        {
            TagColliders(spec);
            return;
        }
        // A moved pointer alone keeps the loaded instances, not a half-typed edit.
        Apply(edited ? spec : mSpec);
        TagColliders(spec);
    }

    void TerrainRuntime::ForgetMeshes()
    {
        if (!Bound())
            return;
        if (mMeshes)
            mMeshes->Forget();
        for (ViewState &view : mViews)
            view.draws.clear();
    }

    void TerrainRuntime::Unload()
    {
        if (!Bound())
            return;
        ForgetMeshes();
        for (InstanceResources &resources : mResources)
            DestroyCollider(resources.collider);
        mResources.clear();
        mWorld->Resource<TerrainWorld>() = TerrainWorld{};
        mSpec = {};
        mPendingSpec = {};
        mReported.clear();
    }

    void TerrainRuntime::Apply(TerrainSceneSpec spec)
    {
        TerrainWorld &current = mWorld->Resource<TerrainWorld>();
        TerrainStreamer *streamer = Streamer();
        if (!streamer)
        {
            if (spec.HasTerrain() && !mSettingsReported)
            {
                MIR_LOG_ERROR("Terrain streaming is not configured, so no terrain loads: {}", mSettingsProblem);
                mSettingsReported = true;
            }
            mSpec = std::move(spec);
            return;
        }

        std::vector<TerrainIssue> issues = spec.issues;
        const auto started = std::chrono::steady_clock::now();
        TerrainWorld next = ResolveTerrainWorld(spec, mReleasesRoot, &current, issues, *streamer);
        const double resolveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        for (const TerrainIssue &issue : issues)
            Report(issue.message);

        mSpec = std::move(spec);

        if (next.SameDerivedState(current))
        {
            current = std::move(next);
            return;
        }

        // Build everything first, then swap: old colliders and meshes go, then the resource.
        std::vector<InstanceResources> old = std::move(mResources);
        mResources = Build(next, old);
        for (InstanceResources &stale : old)
            DestroyCollider(stale.collider);
        std::vector<TerrainReleaseInfo> keep;
        for (const InstanceResources &resources : mResources)
            keep.push_back(resources.release);
        if (mMeshes)
            mMeshes->RetireReleases(keep);
        current = std::move(next);

        for (size_t i = 0; i < current.Instances().size(); ++i)
        {
            const TerrainInstanceState &instance = current.Instances()[i];
            const TerrainReleaseInfo &info = instance.release->Info();
            const TerrainIndex &index = instance.release->Handle().Index();
            const TileRange tiles = instance.release->Layout().Tiles();
            const glm::dvec3 at(instance.placement.translation.Meters());
            MIR_LOG_INFO("Terrain '{}' (asset {:016x}): release {:016x}, {}x{} tiles and {} Regions in {} levels "
                         "drawn per viewport, {}, placed at ({}, {}, {})",
                         instance.recipePath, info.asset.value, info.release.value, tiles.max.x - tiles.min.x + 1,
                         tiles.max.z - tiles.min.z + 1, index.regions.size(),
                         index.regionGrid ? index.regionGrid->Levels() : 0,
                         mResources[i].collider.IsValid() ? "one height field" : "no collider", at.x, at.y, at.z);
        }
        const TerrainStreamingStats stats = streamer->Stats();
        MIR_LOG_INFO("Terrain streaming: {} payloads ({:.2f} MiB) resident of {:.0f} MiB; resolving and the Eager "
                     "wait took {:.3f} s",
                     stats.ready, MiB(stats.residentBytes), MiB(stats.budgetBytes), resolveSeconds);
        if (stats.overBudgetBytes > 0)
            MIR_LOG_WARN("Terrain streaming: Eager payloads exceed the CPU budget by {:.2f} MiB",
                         MiB(stats.overBudgetBytes));
    }

    std::vector<TerrainRuntime::InstanceResources> TerrainRuntime::Build(TerrainWorld &next,
                                                                         std::vector<InstanceResources> &old)
    {
        std::vector<InstanceResources> built;
        for (size_t i = 0; i < next.Instances().size();)
        {
            const TerrainInstanceState &instance = next.Instances()[i];
            if (std::optional<InstanceResources> reused = Reuse(instance, old))
            {
                built.push_back(std::move(*reused));
                ++i;
                continue;
            }
            if (!HasMeshPayloads(instance))
            {
                next.RemoveInstance(i);
                continue;
            }

            InstanceResources resources;
            resources.release = instance.release->Info();
            resources.placement = instance.placement;
            const std::optional<TerrainResidencyMode> mode = Streamer()->ModeOf(instance.release->Info());
            if (mode != TerrainResidencyMode::Eager)
            {
                MIR_LOG_WARN("Terrain '{}': release {:016x} streams by interest ({:.1f} MiB of payloads, {:.0f} MiB "
                             "CPU budget, {} residency); viewports draw it as it loads, and it has no collider until "
                             "collision residency",
                             instance.recipePath, instance.release->Info().release.value,
                             MiB(ReleaseFootprint(instance.release->Handle().Index())),
                             MiB(Streamer()->Settings().cpu.budgetBytes),
                             TerrainResidencyModeName(*Streamer()->Settings().residency.mode));
                built.push_back(std::move(resources));
                ++i;
                continue;
            }

            resources.collider = BuildCollider(instance, resources.colliderOrigin);
            if (resources.collider.IsValid())
            {
                built.push_back(std::move(resources));
                ++i;
                continue;
            }
            // Terrain drawn without its collider is terrain to fall through.
            next.RemoveInstance(i);
        }
        return built;
    }

    std::optional<TerrainRuntime::InstanceResources> TerrainRuntime::Reuse(const TerrainInstanceState &instance,
                                                                            std::vector<InstanceResources> &old)
    {
        const TerrainReleaseInfo &release = instance.release->Info();
        const auto find = [&](bool samePlace)
        {
            return std::find_if(old.begin(), old.end(),
                                [&](const InstanceResources &resources)
                                {
                                    return resources.release == release &&
                                           (!samePlace || SamePlacement(resources.placement, instance.placement));
                                });
        };
        auto match = find(true);
        if (match == old.end())
            match = find(false);
        if (match == old.end())
            return std::nullopt;

        // A failed move leaves it in old, destroyed after the swap like any other.
        if (!SamePlacement(match->placement, instance.placement))
        {
            if (match->collider.IsValid() && !MoveCollider(*match, instance))
                return std::nullopt;
            match->placement = instance.placement;
        }

        InstanceResources reused = std::move(*match);
        old.erase(match);
        return reused;
    }

    bool TerrainRuntime::HasMeshPayloads(const TerrainInstanceState &instance)
    {
        const TerrainIndex &index = instance.release->Handle().Index();
        const TileRange range = instance.release->Layout().Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                const TerrainTileRecord *source = index.Find({x, z});
                if (source && index.Find({x, z}, source->layer, TerrainPayloadKind::LodHeight, 0) &&
                    index.Find({x, z}, source->layer, TerrainPayloadKind::Normal, 0))
                    continue;
                Report(std::format("Terrain '{}': tile ({}, {}) of release {:016x} has no source LOD and normal "
                                   "payloads to draw; the instance is not loaded",
                                   instance.recipePath, x, z, instance.release->Info().release.value));
                return false;
            }
        return true;
    }

    BodyHandle TerrainRuntime::BuildCollider(const TerrainInstanceState &instance, TerrainLocalPosition &origin)
    {
        PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>();
        if (!physics)
        {
            Report(std::format("Terrain '{}': no PhysicsWorld to collide with; the instance is not loaded",
                               instance.recipePath));
            return {};
        }
        const TerrainStreamingSettings::Collision &collision = Streamer()->Settings().collision;
        const TerrainCollisionPolicy defaults{collision.spacingsMeters, collision.maxErrorMeters};
        const auto *configured = mWorld->TryResource<TerrainCollisionPolicy>();
        const auto &policy = configured ? *configured : defaults;
        if (!ValidTerrainCollisionPolicy(policy))
        {
            Report(std::format("Terrain '{}': invalid collision error policy", instance.recipePath));
            return {};
        }
        const TerrainResidentRelease &release = *instance.release;
        std::string refusals;
        for (double spacing : policy.spacings)
        {
            const auto samples = BuildTerrainUniformSamples(release.Handle().Index(), release.Query(), spacing);
            if (!samples)
            {
                refusals += std::format(" [{} m: {}]", spacing, samples.error.message);
                continue;
            }
            const auto &area = samples.value->area;
            const auto worldOrigin = TryTerrainLocalToWorld(area.origin, instance.placement.translation);
            if (!worldOrigin)
            {
                refusals += std::format(" [{} m: origin outside world range]", spacing);
                continue;
            }
            HeightFieldDesc desc;
            desc.mSamples = area.heights;
            desc.mCountX = area.countX;
            desc.mCountZ = area.countZ;
            desc.mSpacing = static_cast<float>(spacing);
            desc.mOrigin = *worldOrigin;
            const BodyHandle body = physics->CreateStaticHeightField(desc);
            const auto info = physics->HeightFieldInfoOf(body);
            const auto error = info ? TerrainCollisionErrorBudget(*samples.value, instance.placement, *worldOrigin,
                info->mMaxQuantizationError, info->mMaxHorizontalError) : std::nullopt;
            const bool covered = info && TerrainCollisionCovers(*samples.value, instance.placement, *worldOrigin,
                info->mExtentX, info->mExtentZ);
            if (covered && error && error->total <= policy.maxError)
            {
                MIR_LOG_INFO("Terrain '{}': collision {} m, error {} m (geometry {}, quantization {}, horizontal {}, numeric {})",
                    instance.recipePath, spacing, error->total, error->geometry, error->quantization,
                    error->horizontal, error->numeric);
                if (!refusals.empty()) MIR_LOG_INFO("Terrain collision refined:{}", refusals);
                origin = area.origin;
                return body;
            }
            physics->DestroyBody(body);
            if (!covered) refusals += std::format(" [{} m: decoded XY support does not cover the map]", spacing);
            refusals += error ? std::format(" [{} m: total {} = geometry {} + quantization {} + horizontal {} + numeric {}]",
                spacing, error->total, error->geometry, error->quantization, error->horizontal, error->numeric)
                : std::format(" [{} m: height field or error proof failed]", spacing);
        }
        Report(std::format("Terrain '{}': no collision spacing satisfies {} m;{} the instance is not loaded",
            instance.recipePath, policy.maxError, refusals));
        return {};
    }

    bool TerrainRuntime::MoveCollider(InstanceResources &resources, const TerrainInstanceState &instance)
    {
        // A new placement changes the numeric error budget and decoded support coverage.
        TerrainLocalPosition origin;
        const BodyHandle body = BuildCollider(instance, origin);
        if (!body.IsValid())
            return false;
        DestroyCollider(resources.collider);
        resources.collider = body;
        resources.colliderOrigin = origin;
        return true;
    }

    void TerrainRuntime::Prepare(std::span<const ViewCamera> views)
    {
        mFrameViews.clear();
        mSelection.clear();
        TerrainStreamer *streamer = Streamer();
        if (!Bound() || !streamer || !mMeshes)
            return;
        mFrameTime = Now();
        const TerrainStreamingSettings::View &settings = streamer->Settings().view;

        // A BLAS only when the project traces; meshes made the other way are made again.
        const RenderSettings *render = mWorld->TryResource<RenderSettings>();
        const bool rayTracing = render && render->rayTracing && mRenderer->RayTracingSupported();
        if (rayTracing != mUploader->RayTracing())
        {
            mMeshes->RetireAll();
            mUploader->SetRayTracing(rayTracing);
        }

        for (const TerrainInstanceState &instance : mWorld->Resource<TerrainWorld>().Instances())
            mSelection.push_back({instance.release->SharedHandle(), instance.placement});
        const TerrainUnitReady ready = Ready();
        mFrameWants.clear();

        for (ViewState &view : mViews)
            view.seen = false;
        for (const ViewCamera &camera : views)
        {
            auto found = std::find_if(mViews.begin(), mViews.end(),
                                      [&](const ViewState &view) { return view.viewport == camera.viewport; });
            if (found == mViews.end())
            {
                ViewState view;
                view.viewport = camera.viewport;
                view.interest = streamer->AddInterest();
                view.selector = std::make_unique<TerrainViewSelector>(settings);
                mViews.push_back(std::move(view));
                found = mViews.end() - 1;
            }
            ViewState &view = *found;
            view.seen = true;

            TerrainView selection;
            selection.frustum = {camera.pose.position, camera.viewProj};
            selection.focalPixels =
                double(camera.extent.height) / (2.0 * std::tan(glm::radians(double(camera.lens.mFovYDegrees)) * 0.5));
            selection.farDistance = camera.lens.HasFarPlane() ? double(camera.lens.mFarZ) : 0.0;
            view.selector->Update(mSelection, selection, ready);
            streamer->SetInterest(view.interest, view.selector->Wants());
            mFrameWants.insert(mFrameWants.end(), view.selector->Wants().begin(), view.selector->Wants().end());
        }

        for (auto it = mViews.begin(); it != mViews.end();)
        {
            if (it->seen)
            {
                ++it;
                continue;
            }
            streamer->RemoveInterest(it->interest);
            it = mViews.erase(it);
        }

        mMeshes->Request(mSelection, mFrameWants, *streamer, mFrameTime);
        ReportMeshFailures();
        for (const ViewCamera &camera : views)
            for (size_t i = 0; i < mViews.size(); ++i)
                if (mViews[i].viewport == camera.viewport)
                {
                    ViewState &view = mViews[i];
                    view.draws.clear();
                    AppendDraws(view.selector->Draws(), view.draws);
                    LogView(view, camera);
                    mFrameViews.push_back(i);
                }
    }

    void TerrainRuntime::AppendViewDraws(size_t view, std::vector<WorldDrawItem> &out)
    {
        if (view >= mFrameViews.size())
            return;
        const std::vector<WorldDrawItem> &draws = mViews[mFrameViews[view]].draws;
        out.insert(out.end(), draws.begin(), draws.end());
    }

    void TerrainRuntime::AppendVolumeDraws(size_t view, WorldPosition origin, const glm::mat4 &viewProj,
                                           std::vector<WorldDrawItem> &out)
    {
        if (view >= mFrameViews.size() || !mMeshes)
            return;
        std::vector<TerrainDrawUnit> units;
        mViews[mFrameViews[view]].selector->Collect({origin, viewProj}, Ready(), units);
        AppendDraws(units, out);
    }

    TerrainUnitReady TerrainRuntime::Ready() const
    {
        return [this](const TerrainDrawUnit &unit) { return mMeshes->Ready(unit.geometry); };
    }

    void TerrainRuntime::AppendDraws(std::span<const TerrainDrawUnit> units, std::vector<WorldDrawItem> &out)
    {
        const std::span<const TerrainInstanceState> instances = mWorld->Resource<TerrainWorld>().Instances();
        for (const TerrainDrawUnit &unit : units)
        {
            if (unit.instance >= instances.size())
                continue;
            const TerrainResidentMesh *mesh = mMeshes->Acquire(unit.geometry, mFrameTime);
            if (!mesh)
                continue;
            if (std::optional<WorldDrawItem> draw = MakeTerrainDraw(*mesh, instances[unit.instance].placement))
                out.push_back(*draw);
        }
    }

    void TerrainRuntime::ReportMeshFailures()
    {
        const std::span<const TerrainInstanceState> instances = mWorld->Resource<TerrainWorld>().Instances();
        for (const auto &[key, error] : mMeshes->TakeFailures())
        {
            const auto instance = std::find_if(instances.begin(), instances.end(),
                                               [&](const TerrainInstanceState &i)
                                               {
                                                   return i.release->Info().asset == key.asset &&
                                                          i.release->Info().release == key.release;
                                               });
            Report(std::format("Terrain '{}': no mesh for {} ({}); that area is not drawn",
                               instance == instances.end() ? std::string("?") : instance->recipePath,
                               DescribeTerrainPayloadKey(key), error));
        }
    }

    void TerrainRuntime::LogView(ViewState &view, const ViewCamera &camera)
    {
        if (mFrameTime - view.loggedAt < kTerrainViewLogSeconds)
            return;
        const TerrainViewStats &s = view.selector->Stats();
        std::map<std::pair<TerrainUnitKind, uint32_t>, size_t> byLevel;
        for (const TerrainDrawUnit &unit : view.selector->Draws())
            ++byLevel[{unit.kind, unit.geometry.level}];
        std::string levels;
        for (const auto &[key, count] : byLevel)
            levels += std::format("{}{} {} L{}", levels.empty() ? "" : ", ", count,
                                  key.first == TerrainUnitKind::Tile ? "tile" : "Region", key.second);
        const TerrainMeshStats m = mMeshes->Stats();
        std::string summary = std::format(
            "{} tiles and {} Regions drawn [{}] ({} coarser and {} other stand-ins, {} areas loading); wants {} "
            "visible and {} prefetch payloads ({:.2f} MiB, {} past the prefetch budget); GPU meshes {} ready, {} "
            "uploading, {} building or built",
            s.drawnTiles, s.drawnRegions, levels, s.coarserFallbacks, s.otherFallbacks, s.missing, s.visibleWants,
            s.prefetchWants, MiB(s.prefetchBytes), s.prefetchDropped, m.ready, m.uploading, m.building + m.built);
        if (summary == view.logged)
            return;
        view.logged = std::move(summary);
        view.loggedAt = mFrameTime;
        const TerrainStreamingStats streaming = Streamer()->Stats();
        const MeshStreamingStats uploads = mRenderer->MeshStreaming();
        const glm::dvec3 eye(camera.pose.position.Meters());
        MIR_LOG_INFO("Terrain view {} at ({:.1f}, {:.1f}, {:.1f}), {}x{} px, {} degrees: {}; streamer {} ready, {} "
                     "loading, {:.2f} MiB resident; GPU {:.2f} of {:.0f} MiB held, {:.2f} MiB retiring, {} built, {} "
                     "retired, {} evicted; renderer {:.2f} MiB uploaded, {} device and {} queue waits",
                     camera.viewport, eye.x, eye.y, eye.z, camera.extent.width, camera.extent.height,
                     camera.lens.mFovYDegrees, view.logged, streaming.ready, streaming.loading,
                     MiB(streaming.residentBytes), MiB(m.heldBytes), MiB(m.budgetBytes), MiB(m.retiringBytes),
                     m.buildsStarted, m.retired, m.evicted, MiB(uploads.uploadedBytes), uploads.deviceWaits,
                     uploads.queueWaits);
    }

    void TerrainRuntime::TagColliders(const TerrainSceneSpec &spec)
    {
        PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>();
        if (!physics)
            return;
        for (const InstanceResources &resources : mResources)
        {
            if (!resources.collider.IsValid())
                continue;
            for (const TerrainInstanceSpec &instance : spec.instances)
                if (SamePlacement(resources.placement, {instance.asset, instance.translation}))
                {
                    physics->SetBodyEntity(resources.collider, instance.entity);
                    break;
                }
        }
    }

    void TerrainRuntime::DestroyCollider(BodyHandle body)
    {
        if (!body.IsValid())
            return;
        if (PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>())
            physics->DestroyBody(body);
    }

    void TerrainRuntime::Report(const std::string &message)
    {
        if (mReported.insert(message).second)
            MIR_LOG_ERROR("{}", message);
    }
}
