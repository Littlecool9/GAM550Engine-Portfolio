/**
 * @file TerrainGenerator.cpp
 * @author Jackey Cen
 * @brief Source heights of a recipe as a pure function of terrain-local sample positions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainGenerator.h"

#include <algorithm>
#include <cmath>

namespace mir::terrainbake
{
    namespace
    {
        // Each term draws from its own stream of the recipe seed.
        uint64_t TermSeed(uint64_t seed, uint64_t term)
        {
            return MixBits(seed + term * 0xD1B54A32D192ED03ull);
        }

        // max(h, floor), rounded within blend; exactly floor below floor - blend.
        double SmoothFloor(double h, double floor, double blend)
        {
            const double hard = std::max(h, floor);
            const double gap = std::abs(h - floor);
            if (blend <= 0.0 || gap >= blend)
                return hard;
            const double t = (blend - gap) / blend;
            return hard + t * t * blend * 0.25;
        }

        SampleRange MapSamples(const TerrainLayout &layout)
        {
            const TileRange tiles = layout.Tiles();
            const int64_t n = layout.IntervalsPerTile();
            return {{layout.TileFirstSample(tiles.min.x), layout.TileFirstSample(tiles.min.z)},
                    {layout.TileFirstSample(tiles.max.x) + n, layout.TileFirstSample(tiles.max.z) + n}};
        }
    }

    size_t TileSampleCount(const TerrainLayout &layout)
    {
        const size_t side = static_cast<size_t>(layout.IntervalsPerTile()) + 1;
        return side * side;
    }

    TerrainGenerator::TerrainGenerator(const TerrainRecipe &recipe)
        : mLayout(recipe.layout), mRelief(recipe.relief),
          mBroad(TermSeed(recipe.seed, 1), recipe.relief.broad.wavelength, recipe.relief.broad.octaves),
          mRidges(TermSeed(recipe.seed, 2), recipe.relief.ridges.wavelength, recipe.relief.ridges.octaves),
          mDetail(TermSeed(recipe.seed, 3), recipe.relief.detail.wavelength, recipe.relief.detail.octaves),
          mExtent(MapSamples(recipe.layout))
    {
    }

    double TerrainGenerator::Height(double x, double z) const
    {
        const ReliefParameters &p = mRelief;
        double h = p.baseHeight + p.tiltX * x + p.tiltZ * z;
        if (p.broad.amplitude != 0.0)
            h += p.broad.amplitude * mBroad.Fbm(x, z);
        if (p.ridges.amplitude != 0.0)
        {
            // Ridges rise from plains level, so lowlands stay plains.
            const double t = std::clamp((h - p.plainsHeight) / p.ridges.amplitude, 0.0, 1.0);
            const double weight = t * t * (3.0 - 2.0 * t);
            if (weight != 0.0)
                h += weight * p.ridges.amplitude * mRidges.Ridged(x, z);
        }
        if (p.detail.amplitude != 0.0)
            h += p.detail.amplitude * mDetail.Fbm(x, z);
        return SmoothFloor(h, p.plainsHeight, p.plainsBlend);
    }

    float TerrainGenerator::SampleHeight(GlobalSampleIndex sample) const
    {
        return static_cast<float>(Height(mLayout.SamplePosition(sample.x), mLayout.SamplePosition(sample.z)));
    }

    SourceFillResult TerrainGenerator::FillTile(TileCoord tile, std::span<float> out) const
    {
        const std::optional<SampleRange> samples = mLayout.TileSamples(tile);
        if (!samples)
            return {SourceFillStatus::OutsideMap, {}};
        return FillRange(*samples, out);
    }

    SourceFillResult TerrainGenerator::FillRange(const SampleRange &range, std::span<float> out) const
    {
        if (range.min.x > range.max.x || range.min.z > range.max.z)
            return {SourceFillStatus::EmptyRange, {}};
        if (range.min.x < mExtent.min.x || range.min.z < mExtent.min.z || range.max.x > mExtent.max.x ||
            range.max.z > mExtent.max.z)
            return {SourceFillStatus::OutsideMap, {}};

            // Both sides are below 2^53, so only the product can overflow.
        const uint64_t width = static_cast<uint64_t>(range.max.x - range.min.x) + 1;
        const uint64_t depth = static_cast<uint64_t>(range.max.z - range.min.z) + 1;
        if (width > out.size() || depth > out.size() / width || width * depth != out.size())
            return {SourceFillStatus::WrongSize, {}};

        size_t index = 0;
        for (int64_t k = range.min.z; k <= range.max.z; ++k)
            for (int64_t i = range.min.x; i <= range.max.x; ++i)
            {
                const float height = SampleHeight({i, k});
                if (!std::isfinite(height))
                    return {SourceFillStatus::NonFinite, {i, k}};
                out[index++] = height;
            }
        return {};
    }
}
