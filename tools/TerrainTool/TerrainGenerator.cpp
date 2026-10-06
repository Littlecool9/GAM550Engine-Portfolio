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
#include <bit>
#include <limits>
#include <vector>

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
        : mLayout(recipe.layout), mFiltered(recipe.recipeVersion != 1), mRelief(recipe.relief),
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

    float TerrainGenerator::SampleHeight(const TerrainLayout &layout, GlobalSampleIndex sample) const
    {
        float height = std::numeric_limits<float>::quiet_NaN();
        FillRange(layout, {sample, sample}, std::span(&height, 1));
        return height;
    }

    SourceFillResult TerrainGenerator::FillTile(const TerrainLayout &layout, TileCoord tile, std::span<float> out) const
    {
        const auto samples = layout.TileSamples(tile);
        return samples ? FillRange(layout, *samples, out) : SourceFillResult{SourceFillStatus::OutsideMap, {}};
    }

    SourceFillResult TerrainGenerator::FillRange(const TerrainLayout &layout, const SampleRange &range,
                                                std::span<float> out) const
    {
        const double ratioValue = layout.SampleSpacing() / mLayout.SampleSpacing();
        if (layout.TileSize() != mLayout.TileSize() || layout.Bounds() != mLayout.Bounds() ||
            !std::isfinite(ratioValue) || ratioValue < 1.0 || ratioValue > mLayout.IntervalsPerTile() ||
            std::floor(ratioValue) != ratioValue || !std::has_single_bit(uint64_t(ratioValue)) ||
            mLayout.IntervalsPerTile() % int64_t(ratioValue) != 0)
            return {SourceFillStatus::InvalidGrid, {}};
        if (layout.SampleSpacing() == mLayout.SampleSpacing())
            return FillRange(range, out);
        if (range.min.x > range.max.x || range.min.z > range.max.z)
            return {SourceFillStatus::EmptyRange, {}};
        const SampleRange extent = MapSamples(layout);
        if (range.min.x < extent.min.x || range.min.z < extent.min.z ||
            range.max.x > extent.max.x || range.max.z > extent.max.z)
            return {SourceFillStatus::OutsideMap, {}};
        const uint64_t width = uint64_t(range.max.x - range.min.x) + 1;
        const uint64_t depth = uint64_t(range.max.z - range.min.z) + 1;
        if (width > out.size() || depth > out.size() / width || width * depth != out.size())
            return {SourceFillStatus::WrongSize, {}};
        if (!mFiltered)
            return {SourceFillStatus::InvalidGrid, {}};

        const uint32_t levels = std::countr_zero(uint64_t(ratioValue));
        SourceFillResult result;
        constexpr int64_t kBlockSide = 32;
        for (int64_t z = range.min.z; z <= range.max.z; z += kBlockSide)
            for (int64_t x = range.min.x; x <= range.max.x; x += kBlockSide)
            {
                std::vector<SampleRange> ranges(levels + 1);
                ranges[levels] = {{x, z}, {std::min(x + kBlockSide - 1, range.max.x),
                                          std::min(z + kBlockSide - 1, range.max.z)}};
                for (uint32_t level = levels; level > 0; --level)
                {
                    const auto &target = ranges[level];
                    ranges[level - 1] = {{target.min.x * 2 - 1, target.min.z * 2 - 1},
                                         {target.max.x * 2 + 1, target.max.z * 2 + 1}};
                }
                SampleRange current = ranges[0];
                size_t sideX = size_t(current.max.x - current.min.x + 1);
                size_t sideZ = size_t(current.max.z - current.min.z + 1);
                std::vector<double> values(sideX * sideZ);
                result.referenceSamples += values.size();
                result.peakDoubleSamples = std::max(result.peakDoubleSamples, uint64_t(values.size()));
                for (size_t k = 0; k < sideZ; ++k)
                    for (size_t i = 0; i < sideX; ++i)
                        values[k * sideX + i] = Height(mLayout.SamplePosition(current.min.x + int64_t(i)),
                                                       mLayout.SamplePosition(current.min.z + int64_t(k)));
                for (uint32_t level = 1; level <= levels; ++level)
                {
                    const auto &target = ranges[level];
                    const size_t nextX = size_t(target.max.x - target.min.x + 1);
                    const size_t nextZ = size_t(target.max.z - target.min.z + 1);
                    std::vector<double> horizontal(nextX * sideZ);
                    std::vector<double> next(nextX * nextZ);
                    result.peakDoubleSamples = std::max(result.peakDoubleSamples,
                        uint64_t(values.size() + horizontal.size() + next.size()));
                    for (size_t k = 0; k < sideZ; ++k)
                        for (size_t i = 0; i < nextX; ++i)
                        {
                            const size_t at = k * sideX + size_t((target.min.x + int64_t(i)) * 2 - current.min.x);
                            horizontal[k * nextX + i] = (values[at - 1] + 2.0 * values[at] + values[at + 1]) * 0.25;
                        }
                    for (size_t k = 0; k < nextZ; ++k)
                        for (size_t i = 0; i < nextX; ++i)
                        {
                            const size_t at = size_t((target.min.z + int64_t(k)) * 2 - current.min.z) * nextX + i;
                            next[k * nextX + i] = (horizontal[at - nextX] + 2.0 * horizontal[at] +
                                                   horizontal[at + nextX]) * 0.25;
                        }
                    values = std::move(next);
                    current = target;
                    sideX = nextX;
                    sideZ = nextZ;
                }
                for (size_t k = 0; k < sideZ; ++k)
                    for (size_t i = 0; i < sideX; ++i)
                    {
                        const float height = static_cast<float>(values[k * sideX + i]);
                        if (!std::isfinite(height))
                            return {SourceFillStatus::NonFinite, {current.min.x + int64_t(i), current.min.z + int64_t(k)}};
                        out[size_t(current.min.z + int64_t(k) - range.min.z) * size_t(width) +
                            size_t(current.min.x + int64_t(i) - range.min.x)] = height;
                    }
            }
        return result;
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
        return {SourceFillStatus::Ok, {}, uint64_t(out.size()), 0};
    }
}
