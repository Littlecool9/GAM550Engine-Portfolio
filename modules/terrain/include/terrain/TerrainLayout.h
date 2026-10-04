/**
 * @file TerrainLayout.h
 * @author Jackey Cen
 * @brief A terrain asset's tile grid, shared sample grid and finite map bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TileKey.h>

#include <cstdint>
#include <optional>

namespace mir
{
    // P1 collides with one Jolt height field over the whole map, which holds at most this many samples a side.
    inline constexpr int64_t kMaxTerrainSamplesPerSide = 32768;

    // Closed rectangle in terrain-local meters. Any finite values, including
    // negative ones and edges that fall inside a tile or between samples.
    struct TerrainLocalBounds
    {
        double minX = 0.0;
        double minZ = 0.0;
        double maxX = 0.0;
        double maxZ = 0.0;
    };

    struct GlobalSampleIndex
    {
        int64_t x = 0;
        int64_t z = 0;

        constexpr bool operator==(const GlobalSampleIndex &) const = default;
    };

    // Inclusive on both ends.
    struct TileRange
    {
        TileCoord min;
        TileCoord max;
    };

    // Inclusive on both ends; neighbours share their edge indices.
    struct SampleRange
    {
        GlobalSampleIndex min;
        GlobalSampleIndex max;
    };

    // One uniform sample spacing s and n sample intervals per tile. Sample i
    // sits at SamplePosition(i) = i * s and tile t spans samples [t*n, (t+1)*n],
    // so tile edges and samples come from one formula and neighbours share
    // their edge samples. Tile size (n * s) is a per-asset value, not a constant.
    //
    // Per-tile spacings (P2) nest only if a coarse spacing is an integer
    // multiple of the fine one and still divides n; dividing the tile size
    // alone is not enough (12 m tiles with 3 m and 4 m samples do not nest).
    class TerrainLayout
    {
    public:
        // Rules: finite bounds with min < max, finite spacing > 0, n >= 1
        [[nodiscard]] static std::optional<TerrainLayout> Create(
            double sampleSpacing, int32_t intervalsPerTile, const TerrainLocalBounds &bounds);

        // n = tileSize / sampleSpacing, which must hold exactly as n * spacing
        // == tileSize in double: 0.3 m tiles of 0.1 m samples are refused, use
        // Create(0.1, 3, ...) instead.
        [[nodiscard]] static std::optional<TerrainLayout> CreateFromTileSize(
            double tileSize, double sampleSpacing, const TerrainLocalBounds &bounds);

        double SampleSpacing() const { return mSpacing; }
        int32_t IntervalsPerTile() const { return mIntervals; }
        double TileSize() const { return static_cast<double>(mIntervals) * mSpacing; }
        const TerrainLocalBounds &Bounds() const { return mBounds; }

        double SamplePosition(int64_t index) const { return static_cast<double>(index) * mSpacing; }
        int64_t TileFirstSample(int32_t tile) const { return static_cast<int64_t>(tile) * mIntervals; }
        int32_t TileOfCell(int64_t cell) const;

        // Tiles holding some of the map. A max edge on a tile edge adds no
        // zero-width tile past it.
        TileRange Tiles() const { return mTiles; }
        bool Contains(TileCoord tile) const;

        // Tile rectangle clipped to the bounds; nullopt outside Tiles().
        std::optional<TerrainLocalBounds> ValidExtent(TileCoord tile) const;
        std::optional<SampleRange> TileSamples(TileCoord tile) const;

        // A sample of a map tile that lies outside the bounds. Stored like any
        // other sample so edge cells have both ends.
        bool IsPaddingSample(GlobalSampleIndex sample) const;

    private:
        TerrainLayout(double spacing, int32_t intervals, const TerrainLocalBounds &bounds);

        double mSpacing = 1.0;
        int32_t mIntervals = 1;
        TerrainLocalBounds mBounds;
        TileRange mTiles;
    };
}
