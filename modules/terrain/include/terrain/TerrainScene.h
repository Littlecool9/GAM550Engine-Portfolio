/**
 * @file TerrainScene.h
 * @author Jackey Cen
 * @brief Resolves a scene's TerrainInstances to placed, pinned and resident releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class World;

    enum class TerrainIssueKind : uint8_t
    {
        MissingRecipe,    // the TerrainInstance names no recipe
        BadRecipePath,    // ResolveTerrainAssetPath refuses the path
        InvalidPlacement, // no Transform, a child, not finite, rotated or scaled
        NoRelease,        // nothing is published for the asset
        BadRelease,       // the release current names cannot be opened, verified or read
        Overlap,          // its effective XZ bounds overlap another instance's
    };

    // The message is one line naming the recipe and what to fix.
    struct TerrainIssue
    {
        TerrainIssueKind kind = TerrainIssueKind::MissingRecipe;
        std::string recipePath;
        std::string message;

        bool operator==(const TerrainIssue &) const = default;
    };

    struct TerrainInstanceSpec
    {
        std::string recipePath;
        // The string the id hashes.
        std::string logicalPath;
        AssetId asset;
        WorldPosition translation;

        bool operator==(const TerrainInstanceSpec &other) const;
    };

    // Sorted by content, not entity, so it compares equal across a Play snapshot restore.
    struct TerrainSceneSpec
    {
        std::vector<TerrainInstanceSpec> instances;
        std::vector<TerrainIssue> issues;

        // Loadable or not.
        bool HasTerrain() const { return !instances.empty() || !issues.empty(); }
        bool operator==(const TerrainSceneSpec &) const = default;
    };

    // Refuses a bad recipe path, and a placement other than a root's translation.
    TerrainSceneSpec CollectTerrainScene(World &world, std::string_view assetIdRoot);

    // Every pack verified and every tile resident; the tiles never change after Load, though Query().HeightAt
    // may still record payload requests.
    class TerrainResidentRelease
    {
    public:
        [[nodiscard]] static TerrainResult<std::shared_ptr<TerrainResidentRelease>> Load(
            const std::filesystem::path &releasesRoot, AssetId asset, TerrainReleaseId release);

        const TerrainReleaseInfo &Info() const { return mHandle.Info(); }
        const TerrainLayout &Layout() const { return mHandle.Layout(); }
        const TerrainReleaseHandle &Handle() const { return mHandle; }
        TerrainHeightQuery &Query() { return mQuery; }
        const TerrainHeightQuery &Query() const { return mQuery; }

        // Valid while this object lives.
        TileSampleLookup Lookup() const;

    private:
        TerrainResidentRelease(TerrainReleaseHandle handle, TerrainHeightQuery query);

        TerrainReleaseHandle mHandle;
        TerrainHeightQuery mQuery;
    };

    // Map bounds plus translation, in world meters.
    struct TerrainWorldRect
    {
        double minX = 0.0;
        double minZ = 0.0;
        double maxX = 0.0;
        double maxZ = 0.0;
    };

    struct TerrainInstanceState
    {
        std::string recipePath;
        TerrainPlacement placement;
        TerrainWorldRect bounds;
        std::shared_ptr<TerrainResidentRelease> release;
    };

    // World resource. App replaces it only at Edit frame boundaries, so a Play keeps its releases.
    class TerrainWorld
    {
    public:
        struct AssetState
        {
            AssetId asset;
            std::string logicalPath;
            // Null when none could load.
            std::shared_ptr<TerrainResidentRelease> release;
            // What current named when last examined, so a poll reacts only when it moves.
            std::optional<TerrainReleaseId> seenCurrent;
            // Reported again instead of re-read.
            std::optional<TerrainIssue> refusal;
        };

        std::span<const TerrainInstanceState> Instances() const { return mInstances; }
        std::span<const AssetState> Assets() const { return mAssets; }
        bool Empty() const { return mInstances.empty(); }

        // On a shared edge, the first instance answers.
        TerrainHeightResult HeightAt(WorldPosition world, double requiredSpacing);

        // True when nothing derived needs rebuilding.
        bool SameDerivedState(const TerrainWorld &other) const;

        // For an instance whose mesh or collider failed.
        void RemoveInstance(size_t index);

    private:
        friend TerrainWorld ResolveTerrainWorld(const TerrainSceneSpec &, const std::filesystem::path &,
                                                const TerrainWorld *, std::vector<TerrainIssue> &);

        std::vector<AssetState> mAssets;
        std::vector<TerrainInstanceState> mInstances;
    };

    // previous is not changed. Overlapping instances are all refused.
    TerrainWorld ResolveTerrainWorld(const TerrainSceneSpec &spec, const std::filesystem::path &releasesRoot,
                                     const TerrainWorld *previous, std::vector<TerrainIssue> &issues);

    // Reads only the pointer files.
    bool TerrainCurrentMoved(const TerrainWorld &world, const std::filesystem::path &releasesRoot);
}
