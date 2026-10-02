/**
 * @file TileHeights.h
 * @author Jackey Cen
 * @brief One tile's final float32 height samples, the block every terrain consumer shares.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TileKey.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mir
{
    // Caps one block at 4097^2 samples (about 64 MiB) so a corrupt record cannot ask for more.
    inline constexpr int32_t kMaxTerrainTileIntervals = 4096;

    // (n + 1)^2 terrain-local heights in meters, row-major with x fastest:
    // sample (i, k) of the layout is at (k - k0) * (n + 1) + (i - i0), where
    // i0 = TileFirstSample(tile.x) and k0 = TileFirstSample(tile.z). Shared edge
    // samples are stored in both neighbours, and padding samples like any other.
    class TileHeights
    {
    public:
        // Zero-filled; nullopt unless 1 <= n <= kMaxTerrainTileIntervals.
        [[nodiscard]] static std::optional<TileHeights> Create(TileCoord tile, int32_t intervalsPerTile);

        // Nullopt unless there are exactly (n + 1)^2 samples.
        [[nodiscard]] static std::optional<TileHeights> Create(TileCoord tile, int32_t intervalsPerTile,
                                                               std::vector<float> samples);

        TileCoord Tile() const { return mTile; }
        int32_t IntervalsPerTile() const { return mIntervals; }
        int32_t SamplesPerSide() const { return mIntervals + 1; }

        std::span<const float> Samples() const { return mSamples; }
        std::span<float> Samples() { return mSamples; }

        // Tile-local indices, 0..n on each axis.
        float At(int32_t i, int32_t k) const;
        void Set(int32_t i, int32_t k, float height);

        bool operator==(const TileHeights &) const = default;

    private:
        TileHeights(TileCoord tile, int32_t intervals, std::vector<float> samples);

        TileCoord mTile;
        int32_t mIntervals = 1;
        std::vector<float> mSamples;
    };
}
