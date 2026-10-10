/**
 * @file TerrainRegions.h
 * @author Jackey Cen
 * @brief Bakes a release's coarse Region hierarchy from its final heights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRecipe.h"

#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TileHeights.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir::terrainbake
{
    // One default chunk per level 0 Region.
    inline constexpr uint32_t kTerrainRegionTiles = 16;
    inline constexpr uint32_t kTerrainRegionCells = 128;

    struct TerrainRegionSettings
    {
        uint32_t regionTiles = kTerrainRegionTiles;
        uint32_t cells = kTerrainRegionCells;

        constexpr bool operator==(const TerrainRegionSettings &) const = default;
    };

    struct TerrainRegionBake
    {
        TerrainRegionWrites writes;
        // In grid ordinal order.
        std::vector<TerrainRegionMesh> meshes;
    };

    // The grid unit is the coarsest source step, so grid lines are nodes of every source. Fails when a Region
    // misses part of its domain or its error cannot be enclosed.
    std::optional<TerrainRegionBake> BakeTerrainRegions(const TerrainRecipe &recipe,
                                                        std::span<const TileHeights> finalHeights,
                                                        const TerrainRegionSettings &settings, std::string &error);

    // Evaluates a Region mesh by locating the cell first; origin-relative x, z.
    class TerrainRegionSampler
    {
    public:
        TerrainRegionSampler(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh);

        std::optional<double> HeightAt(double x, double z) const;

    private:
        const TerrainRegionMesh &mMesh;
        std::vector<double> mLinesX;
        std::vector<double> mLinesZ;
        std::vector<std::vector<uint32_t>> mCells;
    };
}
