/**
 * @file TerrainLayout.cpp
 * @author Jackey Cen
 * @brief A terrain asset's tile grid, shared sample grid and finite map bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainLayout.h"

#include "AxisLocate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        // Integer division rounding toward negative infinity, so cell -1 is in tile -1.
        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            int64_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        bool Finite(const TerrainLocalBounds &b)
        {
            return std::isfinite(b.minX) && std::isfinite(b.minZ) && std::isfinite(b.maxX) && std::isfinite(b.maxZ);
        }

        // 2^53: past it a double no longer holds every integer, so cell indices
        // would not survive the double to int64 round trip.
        constexpr double kExactIntegerLimit = 9007199254740992.0;

        // Samples must stay 2^16 ulps apart, which leaves about 16 bits for the
        // within-cell fraction. A resolvability check, not a precision guarantee.
        constexpr double kMinSpacingInUlps = 65536.0;
    }

    TerrainLayout::TerrainLayout(double spacing, int32_t intervals, const TerrainLocalBounds &bounds)
        : mSpacing(spacing), mIntervals(intervals), mBounds(bounds)
    {
        using detail::LocateAxis;
        mTiles.min = {TileOfCell(LocateAxis(*this, bounds.minX, bounds.maxX).cell),
                      TileOfCell(LocateAxis(*this, bounds.minZ, bounds.maxZ).cell)};
        mTiles.max = {TileOfCell(LocateAxis(*this, bounds.maxX, bounds.maxX).cell),
                      TileOfCell(LocateAxis(*this, bounds.maxZ, bounds.maxZ).cell)};
    }

    std::optional<TerrainLayout> TerrainLayout::Create(
        double sampleSpacing, int32_t intervalsPerTile, const TerrainLocalBounds &bounds)
    {
        if (!std::isfinite(sampleSpacing) || sampleSpacing <= 0.0 || intervalsPerTile < 1)
            return std::nullopt;
        if (!Finite(bounds) || !(bounds.minX < bounds.maxX) || !(bounds.minZ < bounds.maxZ))
            return std::nullopt;

        // One tile of padding: edge cells and edge tiles reach past the bounds.
        const double tileSize = static_cast<double>(intervalsPerTile) * sampleSpacing;
        const double reach = std::max({std::abs(bounds.minX), std::abs(bounds.maxX), std::abs(bounds.minZ),
                                       std::abs(bounds.maxZ)}) +
                             tileSize;
        if (!std::isfinite(reach))
            return std::nullopt;

        const double maxSample = std::ceil(reach / sampleSpacing) + 1.0;
        if (!(maxSample < kExactIntegerLimit))
            return std::nullopt;

        const double ulp = std::nextafter(reach, std::numeric_limits<double>::infinity()) - reach;
        if (sampleSpacing < kMinSpacingInUlps * ulp)
            return std::nullopt;

        // |tile| + 1 must still fit int32 so neighbours of the last tile can be named.
        const double maxTile = std::ceil(maxSample / intervalsPerTile) + 1.0;
        if (!(maxTile < static_cast<double>(std::numeric_limits<int32_t>::max())))
            return std::nullopt;

        return TerrainLayout(sampleSpacing, intervalsPerTile, bounds);
    }

    std::optional<TerrainLayout> TerrainLayout::CreateFromTileSize(
        double tileSize, double sampleSpacing, const TerrainLocalBounds &bounds)
    {
        if (!std::isfinite(tileSize) || !std::isfinite(sampleSpacing) || tileSize <= 0.0 || sampleSpacing <= 0.0)
            return std::nullopt;

        const double ratio = tileSize / sampleSpacing;
        if (!(ratio >= 0.5 && ratio < static_cast<double>(std::numeric_limits<int32_t>::max())))
            return std::nullopt;

        // The quotient rounding to an integer is not enough: n * spacing must
        // rebuild tileSize exactly, because tile edges are computed that way.
        const int64_t intervals = std::llround(ratio);
        if (intervals < 1 || static_cast<double>(intervals) * sampleSpacing != tileSize)
            return std::nullopt;

        return Create(sampleSpacing, static_cast<int32_t>(intervals), bounds);
    }

    int32_t TerrainLayout::TileOfCell(int64_t cell) const
    {
        return static_cast<int32_t>(FloorDiv(cell, mIntervals));
    }

    bool TerrainLayout::Contains(TileCoord tile) const
    {
        return tile.x >= mTiles.min.x && tile.x <= mTiles.max.x && tile.z >= mTiles.min.z && tile.z <= mTiles.max.z;
    }

    std::optional<SampleRange> TerrainLayout::TileSamples(TileCoord tile) const
    {
        if (!Contains(tile))
            return std::nullopt;
        const int64_t n = mIntervals;
        return SampleRange{{tile.x * n, tile.z * n}, {(tile.x + int64_t(1)) * n, (tile.z + int64_t(1)) * n}};
    }

    std::optional<TerrainLocalBounds> TerrainLayout::ValidExtent(TileCoord tile) const
    {
        const std::optional<SampleRange> samples = TileSamples(tile);
        if (!samples)
            return std::nullopt;
        TerrainLocalBounds rect;
        rect.minX = std::max(SamplePosition(samples->min.x), mBounds.minX);
        rect.minZ = std::max(SamplePosition(samples->min.z), mBounds.minZ);
        rect.maxX = std::min(SamplePosition(samples->max.x), mBounds.maxX);
        rect.maxZ = std::min(SamplePosition(samples->max.z), mBounds.maxZ);
        return rect;
    }

    bool TerrainLayout::IsPaddingSample(GlobalSampleIndex sample) const
    {
        const double x = SamplePosition(sample.x);
        const double z = SamplePosition(sample.z);
        return x < mBounds.minX || x > mBounds.maxX || z < mBounds.minZ || z > mBounds.maxZ;
    }

    namespace detail
    {
        AxisCell LocateAxis(const TerrainLayout &layout, double coordinate, double axisMax)
        {
            // The quotient only seeds the search; SamplePosition decides, so the
            // cell always satisfies SamplePosition(cell) <= x < SamplePosition(cell + 1).
            int64_t cell = static_cast<int64_t>(std::floor(coordinate / layout.SampleSpacing()));
            while (coordinate < layout.SamplePosition(cell))
                --cell;
            while (coordinate >= layout.SamplePosition(cell + 1))
                ++cell;

            // The closed max edge belongs to the cell below it, never to one past the map.
            if (coordinate == axisMax && layout.SamplePosition(cell) == axisMax)
                return {cell - 1, 1.0};
            return {cell, (coordinate - layout.SamplePosition(cell)) / layout.SampleSpacing()};
        }
    }
}
