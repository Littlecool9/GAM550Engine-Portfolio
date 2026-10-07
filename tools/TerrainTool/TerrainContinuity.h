/**
 * @file TerrainContinuity.h
 * @author Jackey Cen
 * @brief Seam checks between neighbouring tiles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainReport.h"

#include <terrain/TerrainLayout.h>
#include <terrain/TerrainSurface.h>

#include <cstdint>

namespace mir::terrainbake
{
    // Probes at fractions 0, 1/4, 1/2, 3/4 of each cell, plus the closing end.
    inline constexpr int32_t kEdgeProbesPerCell = 4;

    struct ContinuityStats
    {
        uint32_t edges = 0;
        uint32_t edgesCompared = 0;
        uint32_t corners = 0;
        uint32_t cornersCompared = 0;
        uint64_t probes = 0;
        double maxSurfaceDifference = 0.0;
    };

    // Fills "edges" and "corners"; tiles the lookup lacks are skipped.
    ContinuityStats CheckTileContinuity(const TerrainLayout &layout, const TileSampleLookup &lookup,
                                        TerrainReport &report);
}
