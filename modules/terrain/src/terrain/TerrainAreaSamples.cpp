/**
 * @file TerrainAreaSamples.cpp
 * @author Jackey Cen
 * @brief One release's final samples gathered into a single block for a whole-map collider.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainAreaSamples.h"

#include "AxisLocate.h"
#include "PackInternal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Fail;
    }

    TerrainResult<TerrainAreaSamples> BuildTerrainAreaSamples(const TerrainLayout &layout,
                                                              const TerrainReleaseInfo &release,
                                                              const TileSampleLookup &lookup)
    {
        if (!lookup)
            return Fail<TerrainAreaSamples>(Code::InvalidInput, "no sample lookup");

        // Same cells as BuildTerrainTileMesh.
        const TerrainLocalBounds &bounds = layout.Bounds();
        const int64_t firstX = detail::LocateAxis(layout, bounds.minX, bounds.maxX).cell;
        const int64_t firstZ = detail::LocateAxis(layout, bounds.minZ, bounds.maxZ).cell;
        const int64_t lastX = detail::LocateAxis(layout, bounds.maxX, bounds.maxX).cell + 1;
        const int64_t lastZ = detail::LocateAxis(layout, bounds.maxZ, bounds.maxZ).cell + 1;
        const int64_t countX = lastX - firstX + 1;
        const int64_t countZ = lastZ - firstZ + 1;
        if (countX > kMaxTerrainAreaSamplesPerSide || countZ > kMaxTerrainAreaSamplesPerSide)
            return Fail<TerrainAreaSamples>(Code::Unsupported,
                                            "{} x {} samples is more than one block of {} a side can hold", countX,
                                            countZ, kMaxTerrainAreaSamplesPerSide);

        const std::optional<TerrainLocalPosition> origin =
            TerrainLocalPosition::FromMeters({layout.SamplePosition(firstX), 0.0, layout.SamplePosition(firstZ)});
        if (!origin)
            return Fail<TerrainAreaSamples>(Code::InvalidInput, "the first sample's position is not finite");

        TerrainAreaSamples area;
        area.release = release;
        area.samples = {{firstX, firstZ}, {lastX, lastZ}};
        area.countX = static_cast<uint32_t>(countX);
        area.countZ = static_cast<uint32_t>(countZ);
        area.spacing = layout.SampleSpacing();
        area.origin = *origin;
        area.heights.resize(static_cast<size_t>(countX * countZ));

        const int64_t n = layout.IntervalsPerTile();
        const int64_t side = n + 1;
        const TileRange tiles = layout.Tiles();
        for (int32_t tz = tiles.min.z; tz <= tiles.max.z; ++tz)
            for (int32_t tx = tiles.min.x; tx <= tiles.max.x; ++tx)
            {
                const TileCoord tile{tx, tz};
                const std::span<const float> block = lookup(tile);
                if (block.empty())
                    return Fail<TerrainAreaSamples>(Code::NotFound, "tile {} has no samples", Describe(tile));
                if (block.size() != static_cast<size_t>(side * side))
                    return Fail<TerrainAreaSamples>(Code::Corrupt, "tile {} has {} samples, expected {}",
                                                    Describe(tile), block.size(), side * side);

                // A tile owns its samples up to its upper edges; the last tile owns the closed edge.
                const int64_t i0 = layout.TileFirstSample(tx);
                const int64_t k0 = layout.TileFirstSample(tz);
                const int64_t iEnd = std::min(lastX, tx == tiles.max.x ? i0 + n : i0 + n - 1);
                const int64_t kEnd = std::min(lastZ, tz == tiles.max.z ? k0 + n : k0 + n - 1);
                for (int64_t k = std::max(firstZ, k0); k <= kEnd; ++k)
                    for (int64_t i = std::max(firstX, i0); i <= iEnd; ++i)
                    {
                        const float height = block[static_cast<size_t>((k - k0) * side + (i - i0))];
                        if (!std::isfinite(height))
                            return Fail<TerrainAreaSamples>(Code::Corrupt, "tile {} sample ({}, {}) is not finite",
                                                            Describe(tile), i, k);
                        area.heights[static_cast<size_t>((k - firstZ) * countX + (i - firstX))] = height;
                    }
            }

        TerrainResult<TerrainAreaSamples> result;
        result.value = std::move(area);
        return result;
    }
}
