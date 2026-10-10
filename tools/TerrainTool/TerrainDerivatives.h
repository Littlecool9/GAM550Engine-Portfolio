#pragma once
#include "TerrainRecipe.h"
#include "TerrainRegions.h"
#include <terrain/TerrainLod.h>
#include <terrain/TerrainPackWriter.h>
#include <optional>

namespace mir::terrainbake
{
    inline constexpr double kTerrainInitialMaxLodSpacing = 8.0;
    struct TerrainDerivatives
    {
        std::vector<TerrainLayout> layers;
        std::vector<TileHeights> queryHeights;
        std::vector<uint32_t> queryLayers;
        std::vector<double> queryErrors;
        std::vector<TerrainPayloadWrite> payloads;
        // Baked only when settings are given.
        std::optional<TerrainRegionBake> regions;
    };
    std::optional<TerrainDerivatives> BakeTerrainDerivatives(const TerrainRecipe &recipe,
        std::span<const TileHeights> finalHeights, std::string &error, const TerrainRegionSettings *regions = nullptr);
}
