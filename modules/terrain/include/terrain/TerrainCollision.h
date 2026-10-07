#pragma once

#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainPlacement.h>
#include <array>

namespace mir
{
    struct TerrainCollisionPolicy
    {
        std::array<double, 4> spacings{8.0, 4.0, 2.0, 1.0};
        double maxError = 0.05;
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
    bool TerrainCollisionCovers(const TerrainUniformSamples &samples, const TerrainPlacement &placement,
        WorldPosition origin, double extentX, double extentZ);
    std::optional<TerrainCollisionError> TerrainCollisionErrorBudget(const TerrainUniformSamples &samples,
        const TerrainPlacement &placement, WorldPosition origin, double quantizationError,
        double horizontalError);
}
