/**
 * @file TerrainScene.cpp
 * @author Jackey Cen
 * @brief Resolves a scene's TerrainInstances to placed, pinned and resident releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainScene.h"

#include "PackInternal.h"

#include <assets/AssetPath.h>
#include <core/ecs/Query.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/components/TerrainInstance.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <tuple>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Hex;

        std::string FixedString(const char (&chars)[kFieldStringCapacity])
        {
            return std::string(chars, strnlen(chars, kFieldStringCapacity));
        }

        // "TerrainInstance 'terrain/a.terrain.json' on entity 'Ground'"
        std::string Subject(World &world, Entity entity, const std::string &recipe)
        {
            std::string who = std::format("entity #{}", entity.mIndex);
            if (const Name *name = world.GetComponent<Name>(entity); name && name->mValue[0] != '\0')
                who = std::format("entity '{}'", FixedString(name->mValue));
            if (recipe.empty())
                return std::format("TerrainInstance on {}", who);
            return std::format("TerrainInstance '{}' on {}", recipe, who);
        }

        TerrainIssue Issue(TerrainIssueKind kind, std::string recipe, std::string message)
        {
            return TerrainIssue{kind, std::move(recipe), std::move(message)};
        }

        std::optional<TerrainIssue> CheckPlacement(World &world, Entity entity, const std::string &recipe,
                                                   const std::string &subject)
        {
            const Transform *transform = world.GetComponent<Transform>(entity);
            if (!transform)
                return Issue(TerrainIssueKind::InvalidPlacement, recipe,
                             subject + " has no Transform; its position is the terrain's placement");
            if (ParentOf(world, entity) != kNullEntity)
                return Issue(TerrainIssueKind::InvalidPlacement, recipe,
                             subject + " is a child entity; a terrain instance must be a root, whose position "
                                       "is its world placement");

            // Exact, so an editor leftover is reported, not rounded away; q and -q are equal.
            const glm::quat q = transform->Rotation();
            if (q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || (q.w != 1.0f && q.w != -1.0f))
                return Issue(TerrainIssueKind::InvalidPlacement, recipe,
                             std::format("{} is rotated (quaternion x, y, z, w = {}, {}, {}, {}); terrain is placed "
                                         "by translation only, so reset its rotation to identity",
                                         subject, q.x, q.y, q.z, q.w));
            const glm::vec3 s = transform->Scale();
            if (s != glm::vec3(1.0f))
                return Issue(TerrainIssueKind::InvalidPlacement, recipe,
                             std::format("{} is scaled ({}, {}, {}); terrain is placed by translation only, so "
                                         "reset its scale to (1, 1, 1)",
                                         subject, s.x, s.y, s.z));
            return std::nullopt;
        }

        TerrainWorldRect Rect(const TerrainLayout &layout, WorldPosition translation)
        {
            const glm::dvec3 t(translation.Meters());
            const TerrainLocalBounds &b = layout.Bounds();
            return {t.x + b.minX, t.z + b.minZ, t.x + b.maxX, t.z + b.maxZ};
        }

        // Sharing an edge or a corner is not an overlap.
        bool Overlaps(const TerrainWorldRect &a, const TerrainWorldRect &b)
        {
            return a.minX < b.maxX && b.minX < a.maxX && a.minZ < b.maxZ && b.minZ < a.maxZ;
        }

        std::string DescribePlacement(const TerrainInstanceState &instance)
        {
            const glm::dvec3 t(instance.placement.translation.Meters());
            return std::format("'{}' at ({}, {}, {}) covering x [{}, {}], z [{}, {}]", instance.recipePath, t.x, t.y,
                               t.z, instance.bounds.minX, instance.bounds.maxX, instance.bounds.minZ,
                               instance.bounds.maxZ);
        }

        template <typename States>
        auto FindAsset(States &states, AssetId asset) -> decltype(&states.front())
        {
            for (auto &state : states)
                if (state.asset == asset)
                    return &state;
            return nullptr;
        }

        TerrainWorld::AssetState ResolveAsset(const TerrainInstanceSpec &instance,
                                              const std::filesystem::path &releasesRoot,
                                              const TerrainWorld::AssetState *before,
                                              std::vector<TerrainIssue> &issues)
        {
            TerrainWorld::AssetState state;
            state.asset = instance.asset;
            state.logicalPath = instance.logicalPath;

            const std::shared_ptr<TerrainResidentRelease> kept = before ? before->release : nullptr;
            const std::string keeping =
                kept ? std::format("; keeping release {}", Hex(kept->Info().release.value)) : std::string();
            const std::string subject =
                std::format("Terrain '{}' (asset {})", instance.recipePath, Hex(instance.asset.value));

            const auto refuse = [&](TerrainIssueKind kind, std::string message)
            {
                state.refusal = Issue(kind, instance.recipePath, std::move(message));
                issues.push_back(*state.refusal);
                state.release = kept;
                return state;
            };

            const TerrainResult<TerrainReleaseId> current = PeekTerrainCurrentPointer(releasesRoot, instance.asset);
            if (!current)
            {
                if (current.error.code == Code::NotFound)
                    return refuse(TerrainIssueKind::NoRelease,
                                  std::format("{} has no published release in {}; build the game so "
                                              "MjolnirEngine_sync_terrain publishes its recipe{}",
                                              subject, TerrainAssetReleasesDir(releasesRoot, instance.asset)
                                                           .generic_string(),
                                              keeping));
                return refuse(TerrainIssueKind::BadRelease,
                              std::format("{}: {}{}", subject, current.error.message, keeping));
            }

            state.seenCurrent = *current.value;
            if (before && before->seenCurrent == state.seenCurrent)
            {
                state.release = before->release;
                state.refusal = before->refusal;
                if (state.refusal)
                    issues.push_back(*state.refusal);
                return state;
            }
            if (kept && kept->Info().release == *current.value)
            {
                state.release = kept;
                return state;
            }

            TerrainResult<std::shared_ptr<TerrainResidentRelease>> loaded =
                TerrainResidentRelease::Load(releasesRoot, instance.asset, *current.value);
            if (!loaded)
                return refuse(TerrainIssueKind::BadRelease,
                              std::format("{}: release {} cannot be used: {}{}", subject,
                                          Hex(current.value->value), loaded.error.message, keeping));
            state.release = std::move(*loaded.value);
            return state;
        }
    }

    bool TerrainInstanceSpec::operator==(const TerrainInstanceSpec &other) const
    {
        return recipePath == other.recipePath && logicalPath == other.logicalPath && asset == other.asset &&
               translation.Meters() == other.translation.Meters();
    }

    TerrainSceneSpec CollectTerrainScene(World &world, std::string_view assetIdRoot)
    {
        TerrainSceneSpec spec;
        world.GetOrCreateQuery<const TerrainInstance>().ForEach(
            [&](Entity entity, const TerrainInstance &instance)
            {
                const std::string recipe = FixedString(instance.recipePath);
                const std::string subject = Subject(world, entity, recipe);
                if (recipe.empty())
                {
                    spec.issues.push_back(Issue(TerrainIssueKind::MissingRecipe, recipe,
                                                subject + " names no recipe; set recipePath to a "
                                                          "terrain/<name>.terrain.json under the game's assetsRoot"));
                    return;
                }

                const TerrainAssetPath path = ResolveTerrainAssetPath(assetIdRoot, recipe);
                if (!path.Ok())
                {
                    spec.issues.push_back(Issue(TerrainIssueKind::BadRecipePath, recipe,
                                                subject + ": " +
                                                    DescribeTerrainAssetPathError(path, assetIdRoot, recipe)));
                    return;
                }

                if (std::optional<TerrainIssue> issue = CheckPlacement(world, entity, recipe, subject))
                {
                    spec.issues.push_back(std::move(*issue));
                    return;
                }

                const Transform &transform = *world.GetComponent<Transform>(entity);
                spec.instances.push_back({recipe, path.logicalPath, path.asset,
                                          WorldPosition::FromMeters(transform.Position()).value_or(WorldPosition{})});
            });

        std::sort(spec.instances.begin(), spec.instances.end(),
                  [](const TerrainInstanceSpec &a, const TerrainInstanceSpec &b)
                  {
                      const WorldVec3 ta = a.translation.Meters(), tb = b.translation.Meters();
                      return std::tie(a.recipePath, ta.x, ta.y, ta.z) < std::tie(b.recipePath, tb.x, tb.y, tb.z);
                  });
        std::sort(spec.issues.begin(), spec.issues.end(),
                  [](const TerrainIssue &a, const TerrainIssue &b)
                  { return std::tie(a.recipePath, a.message) < std::tie(b.recipePath, b.message); });
        return spec;
    }

    TerrainResidentRelease::TerrainResidentRelease(TerrainReleaseHandle handle, TerrainHeightQuery query)
        : mHandle(std::move(handle)), mQuery(std::move(query))
    {
    }

    TerrainResult<std::shared_ptr<TerrainResidentRelease>> TerrainResidentRelease::Load(
        const std::filesystem::path &releasesRoot, AssetId asset, TerrainReleaseId release)
    {
        using Result = std::shared_ptr<TerrainResidentRelease>;
        TerrainResult<TerrainReleaseHandle> opened = OpenTerrainRelease(releasesRoot, asset, release);
        if (!opened)
            return detail::Failure<Result>(opened.error, "");

        // A release that is not entirely intact is not used at all.
        const TerrainPackError verified = opened.value->VerifyAllPacks();
        if (verified.code != Code::None)
            return detail::Failure<Result>(verified, "");

        TerrainHeightQuery query = TerrainHeightQuery::Create(*opened.value);
        const TerrainPreloadReport preload = PreloadAllTerrainTiles(query, *opened.value);
        if (!preload.Ok())
            return detail::Failure<Result>(
                preload.firstError, std::format("{} of {} tiles did not load: ", preload.failed,
                                                preload.loaded + preload.failed));

        TerrainResult<Result> result;
        result.value = Result(new TerrainResidentRelease(std::move(*opened.value), std::move(query)));
        return result;
    }

    TileSampleLookup TerrainResidentRelease::Lookup() const
    {
        return [this](TileCoord tile) -> std::span<const float>
        {
            const std::shared_ptr<const TileHeights> heights = mQuery.ResidentTile(0, tile);
            return heights ? heights->Samples() : std::span<const float>();
        };
    }

    TerrainHeightResult TerrainWorld::HeightAt(WorldPosition world, double requiredSpacing)
    {
        for (const TerrainInstanceState &instance : mInstances)
        {
            TerrainHeightResult result = instance.release->Query().HeightAt(
                world, instance.placement, TerrainHeightRequest{instance.release->Info(), requiredSpacing});
            if (result.status != TerrainHeightStatus::OutsideMap)
                return result;
        }
        TerrainHeightResult outside;
        outside.status = TerrainHeightStatus::OutsideMap;
        return outside;
    }

    bool TerrainWorld::SameDerivedState(const TerrainWorld &other) const
    {
        return std::equal(mInstances.begin(), mInstances.end(), other.mInstances.begin(), other.mInstances.end(),
                          [](const TerrainInstanceState &a, const TerrainInstanceState &b)
                          {
                              return a.recipePath == b.recipePath && a.placement.asset == b.placement.asset &&
                                     a.placement.translation.Meters() == b.placement.translation.Meters() &&
                                     a.release == b.release;
                          });
    }

    void TerrainWorld::RemoveInstance(size_t index)
    {
        if (index < mInstances.size())
            mInstances.erase(mInstances.begin() + static_cast<std::ptrdiff_t>(index));
    }

    TerrainWorld ResolveTerrainWorld(const TerrainSceneSpec &spec, const std::filesystem::path &releasesRoot,
                                     const TerrainWorld *previous, std::vector<TerrainIssue> &issues)
    {
        TerrainWorld next;
        for (const TerrainInstanceSpec &instance : spec.instances)
        {
            if (FindAsset(next.mAssets, instance.asset))
                continue;
            const TerrainWorld::AssetState *before = previous ? FindAsset(previous->mAssets, instance.asset) : nullptr;
            next.mAssets.push_back(ResolveAsset(instance, releasesRoot, before, issues));
        }

        std::vector<TerrainInstanceState> placed;
        for (const TerrainInstanceSpec &instance : spec.instances)
        {
            const TerrainWorld::AssetState &asset = *FindAsset(next.mAssets, instance.asset);
            if (!asset.release)
                continue;
            placed.push_back({instance.recipePath, TerrainPlacement{instance.asset, instance.translation},
                              Rect(asset.release->Layout(), instance.translation), asset.release});
        }

        std::vector<bool> refused(placed.size(), false);
        for (size_t i = 0; i < placed.size(); ++i)
            for (size_t j = i + 1; j < placed.size(); ++j)
                if (Overlaps(placed[i].bounds, placed[j].bounds))
                {
                    refused[i] = refused[j] = true;
                    issues.push_back(Issue(TerrainIssueKind::Overlap, placed[i].recipePath,
                                           std::format("Terrain {} overlaps terrain {}; instances may touch but not "
                                                       "overlap, so neither loads",
                                                       DescribePlacement(placed[i]), DescribePlacement(placed[j]))));
                }

        for (size_t i = 0; i < placed.size(); ++i)
            if (!refused[i])
                next.mInstances.push_back(std::move(placed[i]));
        return next;
    }

    bool TerrainCurrentMoved(const TerrainWorld &world, const std::filesystem::path &releasesRoot)
    {
        for (const TerrainWorld::AssetState &asset : world.Assets())
        {
            const TerrainResult<TerrainReleaseId> current = PeekTerrainCurrentPointer(releasesRoot, asset.asset);
            const std::optional<TerrainReleaseId> now = current ? current.value : std::nullopt;
            if (now != asset.seenCurrent)
                return true;
        }
        return false;
    }
}
