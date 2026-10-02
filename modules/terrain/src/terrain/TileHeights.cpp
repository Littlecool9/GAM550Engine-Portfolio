/**
 * @file TileHeights.cpp
 * @author Jackey Cen
 * @brief One tile's final float32 height samples, the block every terrain consumer shares.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TileHeights.h"

#include <core/log/Assert.h>

#include <utility>

namespace mir
{
    namespace
    {
        size_t SampleCount(int32_t intervals)
        {
            const size_t side = static_cast<size_t>(intervals) + 1;
            return side * side;
        }
    }

    TileHeights::TileHeights(TileCoord tile, int32_t intervals, std::vector<float> samples)
        : mTile(tile), mIntervals(intervals), mSamples(std::move(samples))
    {
    }

    std::optional<TileHeights> TileHeights::Create(TileCoord tile, int32_t intervalsPerTile)
    {
        if (intervalsPerTile < 1 || intervalsPerTile > kMaxTerrainTileIntervals)
            return std::nullopt;
        return TileHeights(tile, intervalsPerTile, std::vector<float>(SampleCount(intervalsPerTile), 0.0f));
    }

    std::optional<TileHeights> TileHeights::Create(TileCoord tile, int32_t intervalsPerTile,
                                                   std::vector<float> samples)
    {
        if (intervalsPerTile < 1 || intervalsPerTile > kMaxTerrainTileIntervals)
            return std::nullopt;
        if (samples.size() != SampleCount(intervalsPerTile))
            return std::nullopt;
        return TileHeights(tile, intervalsPerTile, std::move(samples));
    }

    float TileHeights::At(int32_t i, int32_t k) const
    {
        MIR_ASSERT(i >= 0 && i <= mIntervals && k >= 0 && k <= mIntervals, "tile sample index out of range");
        return mSamples[static_cast<size_t>(k) * SamplesPerSide() + static_cast<size_t>(i)];
    }

    void TileHeights::Set(int32_t i, int32_t k, float height)
    {
        MIR_ASSERT(i >= 0 && i <= mIntervals && k >= 0 && k <= mIntervals, "tile sample index out of range");
        mSamples[static_cast<size_t>(k) * SamplesPerSide() + static_cast<size_t>(i)] = height;
    }
}
