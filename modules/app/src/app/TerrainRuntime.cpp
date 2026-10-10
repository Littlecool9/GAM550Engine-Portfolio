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
#include <core/log/Assert.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainCollision.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainRenderMesh.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TerrainTileMesh.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <chrono>
#include <format>
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

    void TerrainRuntime::Bind(World &world, VulkanRenderer &renderer, const GameProject &project)
    {
        mWorld = &world;
        mRenderer = &renderer;
        mAssetIdRoot = project.mAssetIdRoot;
        mReleasesRoot = TerrainReleasesRoot(project.mRootDir);

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
        world.EmplaceResource<TerrainStreamer>(settings, std::make_shared<TerrainWorkerPool>(settings.io.workerThreads));
        MIR_LOG_INFO("Terrain streaming: {:.0f} MiB CPU budget, {} read workers, {} residency{}",
                     MiB(settings.cpu.budgetBytes), settings.io.workerThreads,
                     TerrainResidencyModeName(*settings.residency.mode),
                     load.localFileFound ? " (machine-local overrides applied)" : "");
    }

    TerrainStreamer *TerrainRuntime::Streamer() const
    {
        return mWorld ? mWorld->TryResource<TerrainStreamer>() : nullptr;
    }

    void TerrainRuntime::Update()
    {
        if (TerrainStreamer *streamer = Streamer())
            streamer->Update();
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

        if (!edited && !moved && !mMeshesForgotten)
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
        PublishTerrainDraws(*mWorld, {});
        for (InstanceResources &resources : mResources)
            resources.tiles.Forget();
        mMeshesForgotten = !mResources.empty();
    }

    void TerrainRuntime::Unload()
    {
        if (!Bound())
            return;
        PublishTerrainDraws(*mWorld, {});
        for (InstanceResources &resources : mResources)
        {
            resources.tiles.Forget();
            DestroyCollider(resources.collider);
        }
        mResources.clear();
        mWorld->Resource<TerrainWorld>() = TerrainWorld{};
        mSpec = {};
        mPendingSpec = {};
        mMeshesForgotten = false;
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
            if (mMeshesForgotten)
                RebuildMeshes(next);
            current = std::move(next);
            return;
        }

        // Build everything first, then swap: draws, old meshes and colliders, then the resource.
        std::vector<InstanceResources> old = std::move(mResources);
        mResources = Build(next, old);
        Publish();
        std::vector<MeshHandle> meshes; // none left after ForgetMeshes
        for (InstanceResources &stale : old)
        {
            stale.tiles.TakeMeshes(meshes);
            DestroyCollider(stale.collider);
        }
        if (!meshes.empty())
            mRenderer->ReleaseModelResources(meshes, {}, {});
        mMeshesForgotten = false;
        current = std::move(next);

        for (size_t i = 0; i < current.Instances().size(); ++i)
        {
            const TerrainInstanceState &instance = current.Instances()[i];
            const TerrainReleaseInfo &info = instance.release->Info();
            const TileRange tiles = instance.release->Layout().Tiles();
            const glm::dvec3 at(instance.placement.translation.Meters());
            MIR_LOG_INFO("Terrain '{}' (asset {:016x}): release {:016x}, {}x{} tiles as {} meshes and one "
                         "height field, placed at ({}, {}, {})",
                         instance.recipePath, info.asset.value, info.release.value, tiles.max.x - tiles.min.x + 1,
                         tiles.max.z - tiles.min.z + 1, mResources[i].tiles.DrawItems().size(), at.x, at.y, at.z);
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

            const std::optional<TerrainResidencyMode> mode = Streamer()->ModeOf(instance.release->Info());
            if (mode != TerrainResidencyMode::Eager)
            {
                Report(std::format("Terrain '{}': release {:016x} streams by interest ({:.1f} MiB of payloads, "
                                   "{:.0f} MiB CPU budget, {} residency); meshes and colliders still need the whole "
                                   "release resident, so the instance is not loaded",
                                   instance.recipePath, instance.release->Info().release.value,
                                   MiB(ReleaseFootprint(instance.release->Handle().Index())),
                                   MiB(Streamer()->Settings().cpu.budgetBytes),
                                   TerrainResidencyModeName(*Streamer()->Settings().residency.mode)));
                next.RemoveInstance(i);
                continue;
            }

            InstanceResources resources;
            resources.release = instance.release->Info();
            resources.placement = instance.placement;
            if (BuildMeshes(instance, resources.tiles))
            {
                resources.collider = BuildCollider(instance, resources.colliderOrigin);
                if (resources.collider.IsValid())
                {
                    built.push_back(std::move(resources));
                    ++i;
                    continue;
                }
            }
            // A mesh without its collider is terrain to fall through.
            resources.tiles.Destroy(*mRenderer);
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
                                    return resources.release == release && !resources.tiles.Empty() &&
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
            if (!MoveCollider(*match, instance) || !match->tiles.Place(instance.placement))
                return std::nullopt;
            match->placement = instance.placement;
        }

        InstanceResources reused = std::move(*match);
        old.erase(match);
        return reused;
    }

    bool TerrainRuntime::BuildMeshes(const TerrainInstanceState &instance, TerrainRenderTiles &tiles)
    {
        const TerrainStreamer &streamer = *Streamer();
        const TerrainResidentRelease &release = *instance.release;
        const TerrainIndex &index = release.Handle().Index();
        const TileRange range = release.Layout().Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                // The source precision's LOD and normals, already resident; nothing is read here.
                const TerrainTileRecord *source = index.Find({x, z});
                const auto resident = [&](TerrainPayloadKind kind) -> const TerrainTileRecord *
                { return source ? index.Find({x, z}, source->layer, kind, 0) : nullptr; };
                const TerrainTileRecord *lodRecord = resident(TerrainPayloadKind::LodHeight);
                const TerrainTileRecord *normalRecord = resident(TerrainPayloadKind::Normal);
                const auto lod = lodRecord ? streamer.Lod(TerrainPayloadKeyOf(release.Info(), *lodRecord)) : nullptr;
                const auto normals =
                    normalRecord ? streamer.Normals(TerrainPayloadKeyOf(release.Info(), *normalRecord)) : nullptr;
                TerrainResult<TerrainTileMesh> built;
                if (lod && normals)
                    built = MakeTerrainRenderMesh(index, *lod, *normals);
                else
                    built.error = {TerrainPackErrorCode::NotFound, "its LOD and normal payloads are not resident"};
                if (!built)
                {
                    Report(std::format("Terrain '{}': no mesh for tile ({}, {}) ({}); the instance "
                                       "is not loaded",
                                       instance.recipePath, x, z, built.error.message));
                    return false;
                }
                if (!tiles.Add(*mRenderer, *built.value, instance.placement))
                    return false; // Add logged why
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
        const TerrainCollisionPolicy defaults;
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

    void TerrainRuntime::RebuildMeshes(TerrainWorld &world)
    {
        for (size_t i = 0; i < mResources.size();)
        {
            InstanceResources &resources = mResources[i];
            resources.tiles.Forget();
            if (BuildMeshes(world.Instances()[i], resources.tiles))
            {
                ++i;
                continue;
            }
            resources.tiles.Destroy(*mRenderer);
            DestroyCollider(resources.collider);
            mResources.erase(mResources.begin() + static_cast<std::ptrdiff_t>(i));
            world.RemoveInstance(i);
        }
        Publish();
        mMeshesForgotten = false;
    }

    void TerrainRuntime::Publish()
    {
        std::vector<WorldDrawItem> draws;
        for (const InstanceResources &resources : mResources)
        {
            MIR_ASSERT(resources.tiles.Release() == resources.release, "terrain meshes and collider differ in release");
            draws.insert(draws.end(), resources.tiles.DrawItems().begin(), resources.tiles.DrawItems().end());
        }
        PublishTerrainDraws(*mWorld, draws);
    }

    void TerrainRuntime::TagColliders(const TerrainSceneSpec &spec)
    {
        PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>();
        if (!physics)
            return;
        for (const InstanceResources &resources : mResources)
            for (const TerrainInstanceSpec &instance : spec.instances)
                if (SamePlacement(resources.placement, {instance.asset, instance.translation}))
                {
                    physics->SetBodyEntity(resources.collider, instance.entity);
                    break;
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
