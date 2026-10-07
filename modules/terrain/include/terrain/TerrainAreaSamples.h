/**
 * @file TerrainAreaSamples.h
 * @author Jackey Cen
 * @brief One release's final samples gathered into a single block for a whole-map collider.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainSurface.h>

#include <cstdint>
#include <vector>

namespace mir
{
    // One collision shape only; the recipe reference grid has no such cap.
    inline constexpr uint32_t kMaxTerrainAreaSamplesPerSide = static_cast<uint32_t>(kMaxTerrainSamplesPerSide);

    // P1 collides with one shape over the whole map. Derived and rebuildable.
    struct TerrainAreaSamples
    {
        TerrainReleaseInfo release;
        uint32_t surfaceVersion = kTerrainSurfaceVersion;
        // Inclusive: every cell touching the map, as the tile meshes cover it.
        SampleRange samples;
        uint32_t countX = 0;
        uint32_t countZ = 0;
        double spacing = 0.0;
        // samples.min at height 0.
        TerrainLocalPosition origin;
        // x fastest; a shared edge sample comes from its owning tile.
        std::vector<float> heights;
    };

    // NotFound for a missing tile, Corrupt for bad samples, Unsupported past the size limit.
    TerrainResult<TerrainAreaSamples> BuildTerrainAreaSamples(const TerrainLayout &layout,
                                                              const TerrainReleaseInfo &release,
                                                              const TileSampleLookup &lookup);
}
