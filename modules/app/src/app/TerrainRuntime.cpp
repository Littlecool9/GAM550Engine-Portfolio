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
#include <core/log/Assert.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainTileMesh.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        const char *Describe(TerrainMeshStatus status)
        {
            switch (status)
            {
            case TerrainMeshStatus::Ok:
                return "ok";
            case TerrainMeshStatus::InvalidInput:
                return "invalid input";
            case TerrainMeshStatus::MissingTile:
                return "missing tile";
            case TerrainMeshStatus::MissingNeighbour:
                return "missing neighbour";
            case TerrainMeshStatus::InvalidSamples:
                return "invalid samples";
            }
            return "?";
        }

        bool SamePlacement(const TerrainPlacement &a, const TerrainPlacement &b)
        {
            return a.asset == b.asset && a.translation.Meters() == b.translation.Meters();
        }
    }

    void TerrainRuntime::Bind(World &world, VulkanRenderer &renderer, const GameProject &project)
    {
        mWorld = &world;
        mRenderer = &renderer;
        mAssetIdRoot = project.mAssetIdRoot;
        mReleasesRoot = TerrainReleasesRoot(project.mRootDir);
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
        std::vector<TerrainIssue> issues = spec.issues;
        TerrainWorld next = ResolveTerrainWorld(spec, mReleasesRoot, &current, issues);
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
        const TerrainResidentRelease &release = *instance.release;
        const TerrainLayout &layout = release.Layout();
        const TileSampleLookup lookup = release.Lookup();
        const TileRange range = layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                const TerrainMeshResult built = BuildTerrainTileMesh(layout, release.Info(), {x, z}, lookup);
                if (!built.IsOk())
                {
                    Report(std::format("Terrain '{}': no mesh for tile ({}, {}) ({} at tile ({}, {})); the instance "
                                       "is not loaded",
                                       instance.recipePath, x, z, Describe(built.status), built.tile.x, built.tile.z));
                    return false;
                }
                if (!tiles.Add(*mRenderer, *built.mesh, instance.placement))
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

        const TerrainResidentRelease &release = *instance.release;
        const TerrainResult<TerrainAreaSamples> area =
            BuildTerrainAreaSamples(release.Layout(), release.Info(), release.Lookup());
        if (!area)
        {
            Report(std::format("Terrain '{}': no collider ({}); the instance is not loaded", instance.recipePath,
                               area.error.message));
            return {};
        }
        const std::optional<WorldPosition> world = ColliderWorldOrigin(area.value->origin, instance);
        if (!world)
            return {};

        HeightFieldDesc desc;
        desc.mSamples = area.value->heights;
        desc.mCountX = area.value->countX;
        desc.mCountZ = area.value->countZ;
        // Exact for the 1, 2, 4 and 8 m spacings.
        desc.mSpacing = static_cast<float>(area.value->spacing);
        desc.mOrigin = *world;
        const BodyHandle body = physics->CreateStaticHeightField(desc);
        if (!body.IsValid())
            Report(std::format("Terrain '{}': the height field was refused; the instance is not loaded",
                               instance.recipePath));
        else
            origin = area.value->origin;
        return body;
    }

    bool TerrainRuntime::MoveCollider(const InstanceResources &resources, const TerrainInstanceState &instance)
    {
        PhysicsWorld *physics = mWorld->TryResource<PhysicsWorld>();
        if (!physics)
            return false;
        const std::optional<WorldPosition> world = ColliderWorldOrigin(resources.colliderOrigin, instance);
        if (!world)
            return false;
        physics->SetBodyTransform(resources.collider, *world, kIdentityRotation);
        return true;
    }

    std::optional<WorldPosition> TerrainRuntime::ColliderWorldOrigin(const TerrainLocalPosition &origin,
                                                                     const TerrainInstanceState &instance)
    {
        const std::optional<WorldPosition> world = TryTerrainLocalToWorld(origin, instance.placement.translation);
        if (!world)
            Report(std::format("Terrain '{}': the collider origin is past the world range; the instance is not "
                               "loaded",
                               instance.recipePath));
        return world;
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
        // Overlapping instances are refused, so a loaded placement names one instance. One still
        // waiting to settle matches nothing and keeps its tag.
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
