/**
 * @file TerrainRuntime.cpp
 * @author Jackey Cen
 * @brief The scene's terrain meshes, colliders and TerrainWorld, swapped together in Edit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRuntime.h"

#include <core/ecs/EngineMode.h>
#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>
#include <renderer/RenderSettings.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainCollision.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainStreamingSettings.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        static_assert(kTerrainColliderHole == kHeightFieldHole);

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

        class JoltShape final : public TerrainColliderShape
        {
        public:
            explicit JoltShape(SharedHeightField built) : field(std::move(built)) {}
            SharedHeightField field;
        };

        TerrainColliderId ColliderOf(BodyHandle body)
        {
            return TerrainColliderId(body.mId) + 1;
        }

        BodyHandle BodyOf(TerrainColliderId collider)
        {
            return BodyHandle{uint32_t(collider - 1)};
        }

        std::string Describe(const TerrainCollisionStatus &status)
        {
            std::string text = std::format("{} pieces needed, {} ready, {} coarse, {} pending, {} failed", status.needed,
                                           status.ready, status.coarse, status.pending, status.failed);
            if (!status.reason.empty())
                text += ": " + status.reason;
            return text;
        }

        // Empty when every map tile has source LOD and normal payloads to draw.
        std::string MissingMeshPayloads(const TerrainResidentRelease &release)
        {
            const TerrainIndex &index = release.Handle().Index();
            const TileRange range = release.Layout().Tiles();
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                {
                    const TerrainTileRecord *source = index.Find({x, z});
                    if (source && index.Find({x, z}, source->layer, TerrainPayloadKind::LodHeight, 0) &&
                        index.Find({x, z}, source->layer, TerrainPayloadKind::Normal, 0))
                        continue;
                    return std::format("tile ({}, {}) of release {:016x} has no source LOD and normal payloads to draw",
                                       x, z, release.Info().release.value);
                }
            return {};
        }
    }

    // Shapes come from BuildHeightField on the workers; colliders are restricted static bodies.
    class TerrainRuntime::Colliders final : public TerrainColliderPhysics
    {
    public:
        explicit Colliders(TerrainRuntime &owner) : mOwner(owner), mTelemetry(owner.mTelemetry) {}

        std::optional<TerrainBuiltCollider> Build(const TerrainAreaSamples &samples, float quantizationMin,
                                                  float quantizationMax) const override
        {
            HeightFieldDesc desc;
            desc.mSamples = samples.heights;
            desc.mCountX = samples.countX;
            desc.mCountZ = samples.countZ;
            desc.mSpacing = static_cast<float>(samples.spacing);
            desc.mQuantizationMin = quantizationMin;
            desc.mQuantizationMax = quantizationMax;
            const auto start = std::chrono::steady_clock::now();
            SharedHeightField field = BuildHeightField(desc);
            if (mTelemetry)
                mTelemetry->Record("jolt_shape", std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count(), {}, {}, field.Info() != nullptr);
            if (!field.Info())
                return std::nullopt;
            const HeightFieldInfo info = *field.Info();
            return TerrainBuiltCollider{std::make_shared<JoltShape>(std::move(field)), info.mMaxQuantizationError,
                                        info.mMaxHorizontalError, info.mExtentX, info.mExtentZ, info.mMemoryBytes};
        }

        TerrainColliderId Place(const TerrainColliderShape &shape, WorldPosition origin,
                                const TerrainPlacement &placement) override
        {
            PhysicsWorld *physics = mOwner.mWorld->TryResource<PhysicsWorld>();
            if (!physics)
                return 0;
            const BodyHandle body = physics->CreateStaticHeightField(static_cast<const JoltShape &>(shape).field, origin);
            if (!body.IsValid())
                return 0;
            physics->RestrictBody(body);
            physics->SetBodyEntity(body, mOwner.EntityAt(placement));
            return ColliderOf(body);
        }

        void Remove(TerrainColliderId collider) override
        {
            const auto start = std::chrono::steady_clock::now();
            if (PhysicsWorld *physics = mOwner.mWorld->TryResource<PhysicsWorld>())
                physics->DestroyBody(BodyOf(collider));
            if (mTelemetry)
                mTelemetry->Record("jolt_body_retire", std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count());
        }

    private:
        TerrainRuntime &mOwner;
        std::shared_ptr<TerrainTelemetry> mTelemetry;
    };

    void TerrainRuntime::ConfigureTelemetry(std::shared_ptr<TerrainTelemetry> telemetry, TerrainReleaseOpener opener)
    {
        mTelemetry = std::move(telemetry);
        mProfileOpener = std::move(opener);
    }

    nlohmann::json TerrainRuntime::ProfileSnapshot() const
    {
        using Json = nlohmann::json;
        Json out = Json::object();
        if (const TerrainStreamer *streamer = Streamer())
        {
            const auto cpu = streamer->Stats();
            out["cpu"] = {{"residentBytes", cpu.residentBytes}, {"loadingBytes", cpu.loadingBytes},
                           {"accountedBytes", cpu.residentBytes + cpu.loadingBytes},
                           {"peakAccountedBytes", cpu.peakBytes},
                           {"budgetBytes", cpu.budgetBytes}, {"overBudgetBytes", cpu.overBudgetBytes},
                           {"updateMs", cpu.lastUpdateSeconds * 1000.0}, {"reads", cpu.readsStarted},
                           {"failedReads", cpu.readsFailed}, {"retries", cpu.retries},
                           {"bytesRead", cpu.bytesRead}, {"evicted", cpu.evicted}, {"discarded", cpu.discarded}};
        }
        if (mMeshes)
        {
            const auto gpu = mMeshes->Stats();
            out["meshes"] = {{"heldBytes", gpu.heldBytes}, {"retiringBytes", gpu.retiringBytes},
                              {"accountedBytes", gpu.heldBytes + gpu.retiringBytes},
                              {"peakAccountedBytes", gpu.peakBytes},
                              {"budgetBytes", gpu.budgetBytes}, {"budgetWaits", gpu.budgetWaits},
                              {"requestMs", gpu.lastRequestSeconds * 1000.0}, {"building", gpu.building},
                              {"built", gpu.built}, {"uploading", gpu.uploading}, {"ready", gpu.ready},
                              {"discarded", gpu.discarded}};
        }
        if (mCollision)
        {
            const auto collision = mCollision->Stats();
            size_t held = 0;
            for (const auto &[id, guard] : mGuards)
                held += guard.held;
            out["collision"] = {{"heldBytes", collision.heldBytes}, {"budgetBytes", collision.budgetBytes},
                                 {"peakAccountedBytes", collision.peakBytes},
                                 {"overBudgetBytes", collision.overBudgetBytes}, {"heldBodies", held},
                                 {"building", collision.building}, {"ready", collision.ready},
                                 {"updateMs", collision.lastUpdateSeconds * 1000.0},
                                 {"predictionSeconds", mCollision->PredictionSeconds()}};
        }
        out["views"] = Json::array();
        for (const ViewState &view : mViews)
        {
            const auto &stats = view.selector->Stats();
            std::map<std::tuple<uint32_t, int32_t, int32_t>, uint32_t> coverage;
            size_t duplicated = 0;
            for (const auto &unit : view.selector->Draws())
            {
                if (unit.instance >= mSelection.size())
                    continue;
                TileRange tiles{unit.geometry.coord, unit.geometry.coord};
                if (unit.kind == TerrainUnitKind::Region)
                    tiles = mSelection[unit.instance].release->Index().regionGrid->MapTiles(
                        {unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
                for (int64_t z = tiles.min.z; z <= tiles.max.z; ++z)
                    for (int64_t x = tiles.min.x; x <= tiles.max.x; ++x)
                        duplicated += ++coverage[{unit.instance, int32_t(x), int32_t(z)}] > 1;
            }
            out["views"].push_back({{"viewport", view.viewport}, {"drawUnits", view.draws.size()},
                                    {"tiles", stats.drawnTiles}, {"regions", stats.drawnRegions},
                                    {"missing", stats.missing}, {"duplicatedTiles", duplicated},
                                    {"fallbacks", stats.coarserFallbacks + stats.otherFallbacks},
                                    {"prefetchBytes", stats.prefetchBytes}, {"prefetchDropped", stats.prefetchDropped}});
        }
        out["releases"] = Json::array();
        const auto usage = [&](const TerrainReleaseInfo &release, bool staged)
        {
            const auto cpu = Streamer()->Usage(release);
            const auto gpu = mMeshes ? mMeshes->Usage(release) : TerrainReleaseUsage{};
            const auto collision = mCollision ? mCollision->Usage(release) : TerrainReleaseUsage{};
            out["releases"].push_back({{"asset", TerrainIdHex(release.asset.value)},
                                       {"release", TerrainIdHex(release.release.value)}, {"staged", staged},
                                       {"cpuBytes", cpu.bytes}, {"gpuBytes", gpu.bytes},
                                       {"collisionBytes", collision.bytes}});
        };
        if (Streamer())
        {
            for (const auto &asset : mWorld->Resource<TerrainWorld>().Assets())
                if (asset.release)
                    usage(asset.release->Info(), false);
            if (mSwitch)
                for (const auto &release : mSwitch->Staged())
                    usage(release->Info(), true);
        }
        if (mSwitch)
            out["switch"] = {{"switched", mSwitch->Stats().switched}, {"failed", mSwitch->Stats().failed}};
        return out;
    }

    TerrainRuntime::~TerrainRuntime()
    {
        if (!mWorld)
            return;
        if (ViewDrawSources *sources = mWorld->TryResource<ViewDrawSources>())
            std::erase(sources->sources, static_cast<IViewDrawSource *>(this));
        if (PhysicsStepGates *gates = mWorld->TryResource<PhysicsStepGates>())
            std::erase(gates->gates, static_cast<PhysicsStepGate *>(this));
        if (mCollision)
            mCollision->Clear();
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
        world.Resource<TerrainStreamer>().SetTelemetry(mTelemetry);
        MIR_LOG_INFO("Terrain streaming: {:.0f} MiB CPU budget, {} read workers, {} residency{}",
                     MiB(settings.cpu.budgetBytes), settings.io.workerThreads,
                     TerrainResidencyModeName(*settings.residency.mode),
                     load.localFileFound ? " (machine-local overrides applied)" : "");
        MIR_LOG_INFO("Terrain views: refine above {} px, merge below {} px, prefetch to {} px within {:.1f} MiB",
                     settings.view.refineErrorPixels, settings.view.mergeErrorPixels,
                     settings.view.prefetchErrorPixels, MiB(*settings.view.prefetchBudgetBytes));

        mSwitch = std::make_unique<TerrainReleaseSwitch>(mPool, mReleasesRoot, mProfileOpener,
                                                         [this] { return Now(); });
        mStagingVersion = mSwitch->StagingVersion();

        const TerrainStreamingSettings::Collision &collision = settings.collision;
        mColliders = std::make_shared<Colliders>(*this);
        mCollision = std::make_unique<TerrainCollisionResidency>(collision, mPool, mColliders);
        mCollision->SetTelemetry(mTelemetry);
        PhysicsStepGates *gates = world.TryResource<PhysicsStepGates>();
        if (!gates)
            gates = &world.EmplaceResource<PhysicsStepGates>();
        gates->gates.push_back(this);
        std::string spacings;
        for (const double spacing : collision.spacingsMeters)
            spacings += std::format("{}{}", spacings.empty() ? "" : "/", spacing);
        MIR_LOG_INFO("Terrain collision: {} m spacings within {} m, Interest pieces of {} tiles overlapping {} m, "
                     "{} s assumed load latency plus {} s, {} s prefetch, {:.0f} MiB of shapes kept {} s; bodies "
                     "accept coarse pieces within {} m",
                     spacings, collision.maxErrorMeters, collision.pieceTiles, collision.seamOverlapMeters,
                     collision.loadLatencySeconds, collision.latencyMarginSeconds, collision.prefetchSeconds,
                     MiB(collision.budgetBytes), collision.retainSeconds, collision.bodyToleranceMeters);

        const TerrainStreamingSettings::Gpu &gpu = settings.gpu;
        if (!renderer.ConfigureMeshStreaming({.stagingBytes = gpu.stagingBytes, .frameBytes = gpu.uploadBytesPerFrame}))
        {
            MIR_LOG_ERROR("Terrain meshes cannot stream to the GPU, so no terrain is drawn");
            return;
        }
        mUploader = std::make_unique<TerrainGpuUploader>(renderer);
        mMeshes = std::make_unique<TerrainMeshResidency>(gpu, mPool, *mUploader);
        mMeshes->SetTelemetry(mTelemetry);
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
        TerrainStreamer *streamer = Streamer();
        if (streamer)
            streamer->Update();
        if (mSwitch && streamer)
        {
            mSwitch->Update(*streamer);
            if (mSwitch->StagingVersion() != mStagingVersion)
                ApplyResidencies();
        }
        if (mMeshes)
            mMeshes->Install();
        if (mCollision && streamer)
        {
            // No step runs between frames, so colliders may change here.
            mCollision->Install(Now());
            if (!IsSimulating(*mWorld))
                Collide(mWorld->TryResource<PhysicsWorld>(), 0.0);
            ReportCollision();
        }
        ReportSwitch();
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
        if (mSwitch)
            mSwitch->Resume();

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
        const bool moved = poll && mSwitch && mSwitch->Poll(mWorld->Resource<TerrainWorld>());
        std::vector<TerrainReleaseAdoption> ready = mSwitch ? mSwitch->TakeReady() : std::vector<TerrainReleaseAdoption>{};

        if (!edited && !moved && ready.empty())
        {
            TagColliders(spec);
            return;
        }
        // A moved pointer or a switch alone keeps the loaded instances, not a half-typed edit.
        Apply(edited ? spec : mSpec, std::move(ready));
        TagColliders(spec);
    }

    void TerrainRuntime::BeginPlay()
    {
        if (!Bound() || !mSwitch)
            return;
        for (const TerrainWorld::AssetState &asset : mWorld->Resource<TerrainWorld>().Assets())
            if (asset.release)
                MIR_LOG_INFO("Terrain '{}' (asset {:016x}): Play pins release {:016x}", asset.logicalPath,
                             asset.asset.value, asset.release->Info().release.value);
        for (const TerrainReleaseCandidate &candidate : mSwitch->Candidates())
            if (candidate.state != TerrainSwitchState::Failed)
                MIR_LOG_INFO("Terrain '{}' (asset {:016x}): Play does not wait for release {:016x}, which is {}; its "
                             "switch resumes after Stop",
                             candidate.logicalPath, candidate.asset.value, candidate.release.value,
                             TerrainSwitchStateName(candidate.state));
        mSwitch->Suspend();
        ApplyResidencies();
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
        if (mCollision)
            mCollision->Clear();
        if (mSwitch)
        {
            mSwitch->Clear();
            mStagingVersion = mSwitch->StagingVersion();
        }
        for (ViewState &view : mViews)
            view.staged.clear();
        mStagedWarm.clear();
        mStagedProblems.clear();
        mGuards.clear();
        mResources.clear();
        mWorld->Resource<TerrainWorld>() = TerrainWorld{};
        mSpec = {};
        mPendingSpec = {};
        mTagSpec = {};
        mReported.clear();
    }

    void TerrainRuntime::Apply(TerrainSceneSpec spec, std::vector<TerrainReleaseAdoption> adopted)
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
        TerrainResolveOptions options;
        options.open = mProfileOpener;
        options.keepLoaded = true;
        for (const TerrainReleaseAdoption &adoption : adopted)
            options.adopt.push_back(adoption.release);
        const auto started = std::chrono::steady_clock::now();
        TerrainWorld next = ResolveTerrainWorld(spec, mReleasesRoot, &current, issues, *streamer, options);
        const double resolveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        for (const TerrainIssue &issue : issues)
            Report(issue.message);

        mSpec = std::move(spec);

        if (next.SameDerivedState(current))
        {
            current = std::move(next);
            return;
        }

        // What the releases switched away from still hold, taken before they retire.
        struct Leaving
        {
            const TerrainReleaseAdoption *adoption = nullptr;
            TerrainReleaseId from;
            TerrainReleaseUsage cpu, gpu, pieces;
        };
        std::vector<Leaving> leaving;
        for (const TerrainReleaseAdoption &adoption : adopted)
            for (const TerrainWorld::AssetState &asset : current.Assets())
                if (asset.asset == adoption.release->Info().asset && asset.release)
                {
                    const TerrainReleaseInfo &from = asset.release->Info();
                    leaving.push_back({&adoption, from.release, streamer->Usage(from),
                                       mMeshes ? mMeshes->Usage(from) : TerrainReleaseUsage{},
                                       mCollision ? mCollision->Usage(from) : TerrainReleaseUsage{}});
                }

        mResources = Build(next);
        current = std::move(next);
        ApplyResidencies();
        for (const Leaving &left : leaving)
            LogAdoption(*left.adoption, left.from, left.cpu, left.gpu, left.pieces);

        for (size_t i = 0; i < current.Instances().size(); ++i)
        {
            const TerrainInstanceState &instance = current.Instances()[i];
            const TerrainReleaseInfo &info = instance.release->Info();
            const TerrainResidencyMode mode = streamer->ModeOf(info).value_or(TerrainResidencyMode::Interest);
            const TerrainIndex &index = instance.release->Handle().Index();
            const TileRange tiles = instance.release->Layout().Tiles();
            const glm::dvec3 at(instance.placement.translation.Meters());
            MIR_LOG_INFO("Terrain '{}' (asset {:016x}): release {:016x}, {}x{} tiles and {} Regions in {} levels "
                         "drawn per viewport, {} ({:.1f} MiB of payloads), collides {}, placed at ({}, {}, {})",
                         instance.recipePath, info.asset.value, info.release.value, tiles.max.x - tiles.min.x + 1,
                         tiles.max.z - tiles.min.z + 1, index.regions.size(),
                         index.regionGrid ? index.regionGrid->Levels() : 0, TerrainResidencyModeName(mode),
                         MiB(ReleaseFootprint(index)),
                         mode == TerrainResidencyMode::Eager ? "as one height field once its heights are read"
                                                             : "in pieces its sources' motion asks for",
                         at.x, at.y, at.z);
        }
        const TerrainStreamingStats stats = streamer->Stats();
        MIR_LOG_INFO("Terrain streaming: {} payloads ({:.2f} MiB) resident of {:.0f} MiB; resolving took {:.3f} s",
                     stats.ready, MiB(stats.residentBytes), MiB(stats.budgetBytes), resolveSeconds);
    }

    std::vector<TerrainRuntime::InstanceResources> TerrainRuntime::Build(TerrainWorld &next)
    {
        std::vector<InstanceResources> built;
        for (size_t i = 0; i < next.Instances().size();)
        {
            const TerrainInstanceState &instance = next.Instances()[i];
            if (!HasMeshPayloads(instance))
            {
                next.RemoveInstance(i);
                continue;
            }
            built.push_back({instance.release->Info(), instance.placement});
            ++i;
        }
        return built;
    }

    bool TerrainRuntime::HasMeshPayloads(const TerrainInstanceState &instance)
    {
        const std::string missing = MissingMeshPayloads(*instance.release);
        if (missing.empty())
            return true;
        Report(std::format("Terrain '{}': {}; the instance is not loaded", instance.recipePath, missing));
        return false;
    }

    void TerrainRuntime::ApplyResidencies()
    {
        TerrainStreamer *streamer = Streamer();
        if (!streamer)
            return;
        const std::vector<std::shared_ptr<TerrainResidentRelease>> staged =
            mSwitch ? mSwitch->Staged() : std::vector<std::shared_ptr<TerrainResidentRelease>>{};
        std::vector<TerrainReleaseInfo> keep;
        std::vector<TerrainCollisionInstance> active, prewarm;
        for (const TerrainInstanceState &instance : mWorld->Resource<TerrainWorld>().Instances())
        {
            const TerrainReleaseInfo &info = instance.release->Info();
            keep.push_back(info);
            active.push_back({instance.release->SharedHandle(), instance.placement,
                              streamer->ModeOf(info).value_or(TerrainResidencyMode::Interest)});
            for (const std::shared_ptr<TerrainResidentRelease> &release : staged)
                if (release->Info().asset == info.asset)
                {
                    keep.push_back(release->Info());
                    prewarm.push_back({release->SharedHandle(), instance.placement,
                                       streamer->ModeOf(release->Info()).value_or(TerrainResidencyMode::Interest)});
                }
        }
        if (mMeshes)
            mMeshes->RetireReleases(keep);
        if (mCollision)
            mCollision->SetInstances(active, prewarm);
        mStagingVersion = mSwitch ? mSwitch->StagingVersion() : 0;
    }

    bool TerrainRuntime::IsStaged(AssetId asset, TerrainReleaseId release) const
    {
        if (!mSwitch)
            return false;
        for (const std::shared_ptr<TerrainResidentRelease> &staged : mSwitch->Staged())
            if (staged->Info().asset == asset && staged->Info().release == release)
                return true;
        return false;
    }

    void TerrainRuntime::ReportSwitch()
    {
        TerrainStreamer *streamer = Streamer();
        if (!mSwitch || !streamer)
            return;
        for (const std::shared_ptr<TerrainResidentRelease> &release : mSwitch->Staged())
        {
            const TerrainReleaseInfo &info = release->Info();
            auto problem = mStagedProblems.find(info.release.value);
            if (problem == mStagedProblems.end())
                problem = mStagedProblems.emplace(info.release.value, MissingMeshPayloads(*release)).first;
            const auto warm = mStagedWarm.find(info.asset.value);
            TerrainPrewarmReport report = MeasureTerrainPrewarm(info, *streamer, mMeshes.get(), mCollision.get(),
                                                                warm != mStagedWarm.end() && warm->second);
            if (report.failure.empty())
                report.failure = problem->second;
            mSwitch->Report(info.asset, report);
        }
        for (const TerrainSwitchEvent &event : mSwitch->TakeEvents())
            LogSwitch(event);
    }

    void TerrainRuntime::LogSwitch(const TerrainSwitchEvent &event)
    {
        const TerrainReleaseCandidate &c = event.candidate;
        if (mTelemetry)
        {
            const TerrainReleaseInfo release{.asset = c.asset, .release = c.release};
            if (event.kind == TerrainSwitchEvent::Kind::Verified)
                mTelemetry->Record("release_verify", c.verifySeconds, release);
            else if (event.kind == TerrainSwitchEvent::Kind::Ready)
                mTelemetry->Record("release_prepare", c.readyAt - c.startedAt, release);
        }
        const std::string subject = std::format("Terrain '{}' (asset {:016x})", c.logicalPath, c.asset.value);
        switch (event.kind)
        {
        case TerrainSwitchEvent::Kind::Started:
            MIR_LOG_INFO("{}: current names release {:016x} while release {:016x} is loaded; {:016x} stays in use while "
                         "the new one is checked on a worker",
                         subject, c.release.value, c.loaded.value, c.loaded.value);
            break;
        case TerrainSwitchEvent::Kind::Verified:
            MIR_LOG_INFO("{}: release {:016x} verified on a worker in {:.3f} s ({} packs, {:.2f} MiB read whole, "
                         "{:.3f} s after current moved); its working set now loads beside release {:016x}",
                         subject, c.release.value, c.verifySeconds, c.packs, MiB(c.bytesVerified),
                         c.verifiedAt - c.startedAt, c.loaded.value);
            break;
        case TerrainSwitchEvent::Kind::Dropped:
            MIR_LOG_INFO("{}: release {:016x} is no longer switched to ({}); its checks and prewarm are dropped",
                         subject, c.release.value, c.failure);
            break;
        case TerrainSwitchEvent::Kind::Failed:
            Report(std::format("{}: release {:016x} cannot be used: {}; release {:016x} stays in use until current "
                               "moves again",
                               subject, c.release.value, c.failure, c.loaded.value));
            break;
        case TerrainSwitchEvent::Kind::Ready:
            MIR_LOG_INFO("{}: release {:016x} is {} {:.3f} s after current moved; it replaces release {:016x} at the "
                         "next Edit frame boundary",
                         subject, c.release.value, c.warm ? "warm" : "as warm as the budgets let it get",
                         c.readyAt - c.startedAt, c.loaded.value);
            break;
        case TerrainSwitchEvent::Kind::Unreadable:
            Report(std::format("{}: current cannot be read ({}); release {:016x} stays in use", subject, c.failure,
                               c.loaded.value));
            break;
        }
    }

    void TerrainRuntime::LogAdoption(const TerrainReleaseAdoption &adoption, TerrainReleaseId from,
                                     const TerrainReleaseUsage &cpu, const TerrainReleaseUsage &gpu,
                                     const TerrainReleaseUsage &pieces)
    {
        const TerrainReleaseCandidate &c = adoption.candidate;
        std::string stored;
        for (const TerrainStoredRelease &release : c.stored)
            stored += std::format("{}{:016x} ({:.2f} MiB in {} files{})", stored.empty() ? "" : ", ",
                                  release.release.value, MiB(release.bytes), release.files,
                                  release.release == from      ? ", switched from"
                                  : release.release == c.release ? ", now loaded"
                                                                 : "");
        MIR_LOG_INFO("Terrain '{}' (asset {:016x}): switched from release {:016x} to {:016x} at an Edit frame "
                     "boundary, {:.3f} s after current moved; {:016x}'s {} CPU payloads ({:.2f} MiB) go at the next "
                     "update, its {} GPU meshes ({:.2f} MiB) retire once the frames that use them finish, and its {} "
                     "collision pieces left between steps; kept on disk, none deleted: {}",
                     c.logicalPath, c.asset.value, from.value, c.release.value, Now() - c.startedAt, from.value,
                     cpu.ready + cpu.inFlight, MiB(cpu.bytes), gpu.ready + gpu.inFlight, MiB(gpu.bytes),
                     pieces.ready + pieces.inFlight, stored);
    }

    void TerrainRuntime::TagColliders(const TerrainSceneSpec &spec)
    {
        mTagSpec = spec;
        PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>();
        if (!physics || !mCollision)
            return;
        mCollision->ForEachCollider([&](const TerrainPlacement &placement, TerrainColliderId collider)
                                    { physics->SetBodyEntity(BodyOf(collider), EntityAt(placement)); });
    }

    Entity TerrainRuntime::EntityAt(const TerrainPlacement &placement) const
    {
        for (const TerrainInstanceSpec &instance : mTagSpec.instances)
            if (SamePlacement(placement, {instance.asset, instance.translation}))
                return instance.entity;
        return kNullEntity;
    }

    void TerrainRuntime::CollectSources(PhysicsWorld *physics)
    {
        mSources.clear();
        mBodies.clear();
        mGameSources.clear();
        const TerrainStreamingSettings &settings = Streamer()->Settings();
        if (physics)
        {
            const double gravity = glm::length(physics->Gravity());
            mWorld->GetOrCreateQuery<RigidBody>().ForEach(
                [&](Entity entity, RigidBody &rigidBody)
                {
                    if (!rigidBody.body.IsValid() || rigidBody.motion != PhysicsMotion::Dynamic)
                        return;
                    const std::optional<WorldPosition> at = physics->BodyPosition(rigidBody.body);
                    if (!at)
                        return;
                    TerrainCollisionSource source;
                    source.position = *at;
                    source.velocity = glm::dvec3(physics->BodyLinearVelocity(rigidBody.body));
                    source.radius = rigidBody.shape == PhysicsShape::Sphere ? double(rigidBody.radius)
                                                                            : double(glm::length(rigidBody.halfExtent));
                    source.acceleration = gravity;
                    source.tolerance = settings.collision.bodyToleranceMeters;
                    mSources.push_back(source);
                    mBodies.push_back(rigidBody.body);
                    mGuards[rigidBody.body.mId].entity = entity;
                });
        }
        if (const TerrainCollisionSources *game = mWorld->TryResource<TerrainCollisionSources>())
            for (const auto &[id, source] : game->All())
            {
                mSources.push_back(source);
                mGameSources.push_back(id);
            }
    }

    void TerrainRuntime::Collide(PhysicsWorld *physics, double stepSeconds)
    {
        if (const TerrainCollisionPolicy *policy = mWorld->TryResource<TerrainCollisionPolicy>())
        {
            if (ValidTerrainCollisionPolicy(*policy))
                mCollision->SetPolicy(*policy);
            else
                Report("Terrain collision: the World's TerrainCollisionPolicy is invalid, so the streaming settings' "
                       "spacings and error apply");
        }
        CollectSources(physics);
        mCollision->Update(mSources, *Streamer(), Now(), stepSeconds);
        if (TerrainCollisionSources *game = mWorld->TryResource<TerrainCollisionSources>())
            for (size_t i = 0; i < mGameSources.size(); ++i)
                game->SetStatus(mGameSources[i], mCollision->Answer(mBodies.size() + i).status);
    }

    void TerrainRuntime::BeforeStep(World &, PhysicsWorld &physics, float dt)
    {
        if (!mCollision || !Streamer())
            return;
        mCollision->Install(Now());
        Collide(&physics, dt);
        const double now = Now();
        const double gravity = glm::length(physics.Gravity());
        for (auto &[id, guard] : mGuards)
            guard.seen = false;
        for (size_t i = 0; i < mBodies.size(); ++i)
        {
            const BodyHandle body = mBodies[i];
            const TerrainCollisionAnswer &answer = mCollision->Answer(i);
            const TerrainCollisionStatus &status = answer.status;
            Guard &guard = mGuards[body.mId];
            const glm::dvec3 at(mSources[i].position.Meters());
            const double step = (glm::length(mSources[i].velocity) + gravity * dt) * dt;
            const bool arrived = guard.fresh;
            const bool jumped = !arrived && glm::length(at - guard.last) > step + 1.0;
            guard.seen = true;
            guard.fresh = false;
            const bool simulate =
                status.state == TerrainCollisionState::Ready || status.state == TerrainCollisionState::CoarseSufficient;
            const std::string who = NameOf(guard.entity);
            if (!simulate && !physics.IsBodyHeld(body))
            {
                physics.HoldBody(body);
                guard.held = true;
                guard.since = now;
                MIR_LOG_INFO("Terrain collision holds {} at ({:.2f}, {:.2f}, {:.2f}){}: collision {}, {}", who, at.x,
                             at.y, at.z, arrived ? " as it spawns" : jumped ? " after a teleport" : "",
                             TerrainCollisionStateName(status.state), Describe(status));
            }
            else if (simulate && physics.IsBodyHeld(body))
            {
                physics.ResumeBody(body);
                guard.held = false;
                if (mTelemetry)
                    mTelemetry->Record("body_hold", now - guard.since);
                MIR_LOG_INFO("Terrain collision resumes {} after {:.3f} s held: collision {}, contact error {} m, {}",
                             who, now - guard.since, TerrainCollisionStateName(status.state), status.contactError,
                             Describe(status));
            }
            else if (!simulate && guard.status.state != status.state)
                MIR_LOG_INFO("Terrain collision keeps holding {}: collision {}, {}", who,
                             TerrainCollisionStateName(status.state), Describe(status));
            else if (simulate && (arrived || jumped) && status.needed > 0)
                MIR_LOG_INFO("Terrain collision lets {} through{}: collision {}, contact error {} m", who,
                             arrived ? " as it spawns" : " after a teleport", TerrainCollisionStateName(status.state),
                             status.contactError);
            if (answer.unowned && !guard.unowned)
                MIR_LOG_WARN("Terrain collision: {} is wider than the seam overlap lets one piece hold, so it may "
                             "touch the seams between pieces",
                             who);
            guard.unowned = answer.unowned;
            std::vector<BodyHandle> contacts;
            contacts.reserve(answer.contacts.size());
            for (const TerrainColliderId collider : answer.contacts)
                contacts.push_back(BodyOf(collider));
            physics.RestrictContacts(body, contacts);
            guard.status = status;
            guard.last = at;
        }
        std::erase_if(mGuards, [](const auto &entry) { return !entry.second.seen; });
        ReportCollision();
    }

    void TerrainRuntime::ReportCollision()
    {
        for (const TerrainCollisionEvent &event : mCollision->TakeEvents())
        {
            const std::string piece = DescribeTerrainCollisionPiece(event.piece);
            if (mTelemetry)
            {
                const TerrainReleaseInfo release{.asset = event.piece.asset, .release = event.piece.release};
                if (event.kind == TerrainCollisionEvent::Kind::Placed)
                {
                    mTelemetry->Record("collision_build", event.buildSeconds, release, piece);
                }
                else if (event.kind == TerrainCollisionEvent::Kind::Failed)
                    mTelemetry->Record("collision_build", event.buildSeconds, release, piece, false);
            }
            switch (event.kind)
            {
            case TerrainCollisionEvent::Kind::Placed:
                if (event.staged)
                {
                    MIR_LOG_INFO("Terrain collision: {} built ahead of its release switch at {} m, error {} m, in "
                                 "{:.1f} ms, {:.1f} KiB; placed once the release is switched to",
                                 piece, event.spacing, event.error.total, event.buildSeconds * 1000.0,
                                 double(event.bytes) / 1024.0);
                    break;
                }
                MIR_LOG_INFO("Terrain collision: {} placed for {} instances at {} m, error {} m (geometry {}, "
                             "quantization {}, horizontal {}, numeric {}), built in {:.1f} ms, {:.1f} KiB{}",
                             piece, event.colliders, event.spacing, event.error.total, event.error.geometry,
                             event.error.quantization, event.error.horizontal, event.error.numeric,
                             event.buildSeconds * 1000.0, double(event.bytes) / 1024.0,
                             std::isfinite(event.latencySeconds)
                                 ? std::format(", collidable {:.3f} s after it was first needed", event.latencySeconds)
                                 : std::string());
                break;
            case TerrainCollisionEvent::Kind::Retired:
                MIR_LOG_INFO("Terrain collision: {} retired ({}, {:.1f} s since last needed)", piece, event.message,
                             event.unusedSeconds);
                break;
            case TerrainCollisionEvent::Kind::Failed:
                // The switch reports it and keeps the loaded release.
                if (IsStaged(event.piece.asset, event.piece.release))
                    break;
                Report(std::format("Terrain collision: {} failed: {}; bodies that need it are held", piece,
                                   event.message));
                break;
            }
        }
    }

    std::string TerrainRuntime::NameOf(Entity entity) const
    {
        if (const Name *name = mWorld->GetComponent<Name>(entity); name && name->mValue[0] != '\0')
            return std::format("entity '{}'", std::string(name->mValue, strnlen(name->mValue, sizeof(name->mValue))));
        return std::format("entity #{}", entity.mIndex);
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

        // A staged release in place of each loaded one of its asset, selected but never drawn.
        std::map<uint64_t, std::vector<TerrainSelectionInstance>> staged;
        if (mSwitch)
            for (const std::shared_ptr<TerrainResidentRelease> &release : mSwitch->Staged())
                for (const TerrainInstanceState &instance : mWorld->Resource<TerrainWorld>().Instances())
                    if (instance.placement.asset == release->Info().asset)
                        staged[release->Info().asset.value].push_back({release->SharedHandle(), instance.placement});
        mStagedWarm.clear();
        for (const auto &[asset, instances] : staged)
            mStagedWarm[asset] = true;

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
            std::vector<TerrainPayloadWant> wants(view.selector->Wants().begin(), view.selector->Wants().end());

            std::erase_if(view.staged, [&](const auto &entry) { return !staged.contains(entry.first); });
            for (const auto &[asset, instances] : staged)
            {
                std::unique_ptr<TerrainViewSelector> &selector = view.staged[asset];
                if (!selector)
                    selector = std::make_unique<TerrainViewSelector>(settings);
                selector->Update(instances, selection, ready);
                const TerrainViewStats &stats = selector->Stats();
                mStagedWarm[asset] = mStagedWarm[asset] && stats.missing == 0 && stats.coarserFallbacks == 0 &&
                                     stats.otherFallbacks == 0;
                // Below everything the loaded releases want.
                for (const TerrainPayloadWant &want : selector->Wants())
                    if (want.demand == TerrainDemand::Visible)
                        wants.push_back({want.key, TerrainDemand::Prefetch, want.rank});
            }

            streamer->SetInterest(view.interest, wants);
            mFrameWants.insert(mFrameWants.end(), wants.begin(), wants.end());
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

        std::vector<TerrainSelectionInstance> meshInstances = mSelection;
        for (const auto &[asset, instances] : staged)
            meshInstances.insert(meshInstances.end(), instances.begin(), instances.end());
        mMeshes->Request(meshInstances, mFrameWants, *streamer, mFrameTime);
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
            // The switch reports it and keeps the loaded release.
            if (IsStaged(key.asset, key.release))
                continue;
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
        std::string collision;
        if (mCollision)
        {
            const TerrainCollisionStats c = mCollision->Stats();
            collision = std::format("; collision {} pieces ready ({} colliders, {:.2f} MiB), {} building, {} waiting, "
                                    "{} failed, prediction {:.2f} s",
                                    c.ready, c.colliders, MiB(c.heldBytes), c.building, c.waiting, c.failed,
                                    mCollision->PredictionSeconds());
        }
        MIR_LOG_INFO("Terrain view {} at ({:.1f}, {:.1f}, {:.1f}), {}x{} px, {} degrees: {}; streamer {} ready, {} "
                     "loading, {:.2f} MiB resident; GPU {:.2f} of {:.0f} MiB held, {:.2f} MiB retiring, {} built, {} "
                     "retired, {} evicted; renderer {:.2f} MiB uploaded, {} device and {} queue waits{}",
                     camera.viewport, eye.x, eye.y, eye.z, camera.extent.width, camera.extent.height,
                     camera.lens.mFovYDegrees, view.logged, streaming.ready, streaming.loading,
                     MiB(streaming.residentBytes), MiB(m.heldBytes), MiB(m.budgetBytes), MiB(m.retiringBytes),
                     m.buildsStarted, m.retired, m.evicted, MiB(uploads.uploadedBytes), uploads.deviceWaits,
                     uploads.queueWaits, collision);
    }

    void TerrainRuntime::Report(const std::string &message)
    {
        if (mReported.insert(message).second)
            MIR_LOG_ERROR("{}", message);
    }
}
