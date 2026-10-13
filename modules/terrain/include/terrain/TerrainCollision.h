#pragma once

#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainPlacement.h>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace mir
{
    // The streaming settings' collision spacings and error; a World resource of this type overrides them.
    struct TerrainCollisionPolicy
    {
        std::vector<double> spacings;
        double maxError = std::numeric_limits<double>::quiet_NaN();
    };

    bool ValidTerrainCollisionPolicy(const TerrainCollisionPolicy &policy);

    struct TerrainUniformSamples
    {
        TerrainAreaSamples area;
        TerrainLocalBounds validBounds;
        double maxDeviation = 0.0;
        double maxSlope = 0.0;
        double maxAbsHeight = 0.0;
    };

    struct TerrainCollisionError
    {
        double geometry = 0.0;
        double quantization = 0.0;
        double horizontal = 0.0;
        double numeric = 0.0;
        double total = 0.0;
    };

    TerrainResult<TerrainUniformSamples> BuildTerrainUniformSamples(const TerrainIndex &index,
        const TerrainHeightQuery &query, double spacing);

    // A tile's resident final source in a layer, or null.
    using TerrainSourceTileLookup = std::function<std::shared_ptr<const TileHeights>(uint32_t layer, TileCoord tile)>;
    // The part of the map inside bounds, from the sources the lookup gives; any thread when the lookup is.
    TerrainResult<TerrainUniformSamples> BuildTerrainUniformSamples(const TerrainIndex &index,
        const TerrainSourceTileLookup &sources, double spacing, const TerrainLocalBounds &bounds);
    bool TerrainCollisionCovers(const TerrainUniformSamples &samples, const TerrainPlacement &placement,
        WorldPosition origin, double extentX, double extentZ);
    std::optional<TerrainCollisionError> TerrainCollisionErrorBudget(const TerrainUniformSamples &samples,
        const TerrainPlacement &placement, WorldPosition origin, double quantizationError,
        double horizontalError);
}
