/**
 * @file TileHeights.h
 * @author Jackey Cen
 * @brief One tile's final float32 height samples, shared by every terrain consumer.
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
    // 4097^2 samples (64 MiB): caps what a corrupt record can ask for.
    inline constexpr int32_t kMaxTerrainTileIntervals = 4096;

    // (n + 1)^2 heights in meters, x fastest. Edge samples are stored in both neighbours.
    class TileHeights
    {
    public:
    // Zero-filled.
        [[nodiscard]] static std::optional<TileHeights> Create(TileCoord tile, int32_t intervalsPerTile);

        [[nodiscard]] static std::optional<TileHeights> Create(TileCoord tile, int32_t intervalsPerTile,
                                                               std::vector<float> samples);

        TileCoord Tile() const { return mTile; }
        int32_t IntervalsPerTile() const { return mIntervals; }
        int32_t SamplesPerSide() const { return mIntervals + 1; }

        std::span<const float> Samples() const { return mSamples; }
        std::span<float> Samples() { return mSamples; }

    // 0..n on each axis.
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
