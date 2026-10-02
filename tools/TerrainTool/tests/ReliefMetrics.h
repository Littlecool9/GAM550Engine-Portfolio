/**
 * @file ReliefMetrics.h
 * @author Jackey Cen
 * @brief Test-only shape measurements over a generated map.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mir::terrainbake::test
{
    // Padding included, x fastest.
    struct MapGrid
    {
        GlobalSampleIndex origin;
        size_t width = 0;
        size_t depth = 0;
        double spacing = 1.0;
        std::vector<float> heights;
        std::vector<bool> inside; // false for padding samples

        double At(size_t i, size_t k) const { return heights[k * width + i]; }
        bool In(size_t i, size_t k) const { return inside[k * width + i]; }
        bool WindowIn(size_t i0, size_t k0, size_t size) const
        {
            for (size_t k = k0; k < k0 + size; ++k)
                for (size_t i = i0; i < i0 + size; ++i)
                    if (!In(i, k))
                        return false;
            return true;
        }
    };

    inline MapGrid GenerateMap(const TerrainGenerator &generator)
    {
        const TerrainLayout &layout = generator.Layout();
        const TileRange tiles = layout.Tiles();
        const int64_t n = layout.IntervalsPerTile();
        const SampleRange range{{layout.TileFirstSample(tiles.min.x), layout.TileFirstSample(tiles.min.z)},
                                {layout.TileFirstSample(tiles.max.x) + n, layout.TileFirstSample(tiles.max.z) + n}};
        MapGrid grid;
        grid.origin = range.min;
        grid.width = static_cast<size_t>(range.max.x - range.min.x + 1);
        grid.depth = static_cast<size_t>(range.max.z - range.min.z + 1);
        grid.spacing = layout.SampleSpacing();
        grid.heights.resize(grid.width * grid.depth);
        if (!generator.FillRange(range, grid.heights).Ok())
            return {};
        grid.inside.resize(grid.heights.size());
        for (size_t k = 0; k < grid.depth; ++k)
            for (size_t i = 0; i < grid.width; ++i)
                grid.inside[k * grid.width + i] = !layout.IsPaddingSample(
                    {grid.origin.x + static_cast<int64_t>(i), grid.origin.z + static_cast<int64_t>(k)});
        return grid;
    }

    // Windows where every height is equal: flat, not just gentle.
    inline int CountFlatWindows(const MapGrid &grid, size_t size)
    {
        int count = 0;
        for (size_t k = 0; k + size <= grid.depth; ++k)
            for (size_t i = 0; i + size <= grid.width; ++i)
            {
                if (!grid.WindowIn(i, k, size))
                    continue;
                double low = grid.At(i, k);
                double high = low;
                for (size_t b = k; b < k + size; ++b)
                    for (size_t a = i; a < i + size; ++a)
                    {
                        low = std::min(low, grid.At(a, b));
                        high = std::max(high, grid.At(a, b));
                    }
                if (high == low)
                    ++count;
            }
        return count;
    }

    struct PlaneFit
    {
        double grade = 0.0; // |gradient|, m per m
        double rms = 0.0;   // residual, m
    };

    // Least squares over an odd square window.
    inline PlaneFit FitPlane(const MapGrid &grid, size_t i0, size_t k0, size_t size)
    {
        const double half = static_cast<double>(size - 1) / 2.0;
        double mean = 0.0, sx = 0.0, sz = 0.0, uu = 0.0;
        for (size_t k = k0; k < k0 + size; ++k)
            for (size_t i = i0; i < i0 + size; ++i)
            {
                const double u = static_cast<double>(i - i0) - half;
                const double v = static_cast<double>(k - k0) - half;
                const double h = grid.At(i, k);
                mean += h;
                sx += h * u;
                sz += h * v;
                uu += u * u;
            }
        const double count = static_cast<double>(size * size);
        mean /= count;
        const double a = sx / uu;
        const double b = sz / uu;
        double residual = 0.0;
        for (size_t k = k0; k < k0 + size; ++k)
            for (size_t i = i0; i < i0 + size; ++i)
            {
                const double u = static_cast<double>(i - i0) - half;
                const double v = static_cast<double>(k - k0) - half;
                const double r = grid.At(i, k) - (mean + a * u + b * v);
                residual += r * r;
            }
        return {std::sqrt(a * a + b * b) / grid.spacing, std::sqrt(residual / count)};
    }

    inline int CountPlanarSlopes(const MapGrid &grid, size_t size, double minGrade, double maxRms)
    {
        int count = 0;
        for (size_t k = 0; k + size <= grid.depth; ++k)
            for (size_t i = 0; i + size <= grid.width; ++i)
            {
                if (!grid.WindowIn(i, k, size))
                    continue;
                const PlaneFit fit = FitPlane(grid, i, k, size);
                if (fit.grade >= minGrade && fit.rms <= maxRms)
                    ++count;
            }
        return count;
    }

    // Crests: both flanks fall by minFlankGrade and stand minProminence above `reach` away.
    inline int CountRidgeCrests(const MapGrid &grid, size_t reach, double minFlankGrade, double minProminence)
    {
        const double minDrop = minFlankGrade * grid.spacing;
        int count = 0;
        for (size_t k = 0; k < grid.depth; ++k)
            for (size_t i = 0; i < grid.width; ++i)
            {
                if (!grid.In(i, k))
                    continue;
                const double h = grid.At(i, k);
                bool crest = false;
                if (i >= reach && i + reach < grid.width && grid.In(i - reach, k) && grid.In(i + reach, k))
                    crest = h - grid.At(i - 1, k) >= minDrop && h - grid.At(i + 1, k) >= minDrop &&
                            h - (grid.At(i - reach, k) + grid.At(i + reach, k)) / 2.0 >= minProminence;
                if (!crest && k >= reach && k + reach < grid.depth && grid.In(i, k - reach) && grid.In(i, k + reach))
                    crest = h - grid.At(i, k - 1) >= minDrop && h - grid.At(i, k + 1) >= minDrop &&
                            h - (grid.At(i, k - reach) + grid.At(i, k + reach)) / 2.0 >= minProminence;
                if (crest)
                    ++count;
            }
        return count;
    }

    // Spread of block means for blocks fully inside the map.
    inline double BlockMeanSpread(const MapGrid &grid, size_t block)
    {
        double low = 0.0, high = 0.0;
        bool any = false;
        for (size_t k = 0; k + block <= grid.depth; k += block)
            for (size_t i = 0; i + block <= grid.width; i += block)
            {
                if (!grid.WindowIn(i, k, block))
                    continue;
                double sum = 0.0;
                for (size_t b = k; b < k + block; ++b)
                    for (size_t a = i; a < i + block; ++a)
                        sum += grid.At(a, b);
                const double mean = sum / static_cast<double>(block * block);
                low = any ? std::min(low, mean) : mean;
                high = any ? std::max(high, mean) : mean;
                any = true;
            }
        return high - low;
    }

    // BlockMeanSpread after removing the best-fit plane.
    inline double DetrendedBlockSpread(const MapGrid &grid, size_t block)
    {
        struct Mean
        {
            double x, z, h;
        };
        std::vector<Mean> means;
        for (size_t k = 0; k + block <= grid.depth; k += block)
            for (size_t i = 0; i + block <= grid.width; i += block)
            {
                if (!grid.WindowIn(i, k, block))
                    continue;
                double sum = 0.0;
                for (size_t b = k; b < k + block; ++b)
                    for (size_t a = i; a < i + block; ++a)
                        sum += grid.At(a, b);
                means.push_back({static_cast<double>(i), static_cast<double>(k), sum / double(block * block)});
            }
        if (means.size() < 3)
            return 0.0;

        Mean centre{0.0, 0.0, 0.0};
        for (const Mean &m : means)
        {
            centre.x += m.x;
            centre.z += m.z;
            centre.h += m.h;
        }
        const double count = static_cast<double>(means.size());
        centre = {centre.x / count, centre.z / count, centre.h / count};
        double sxx = 0.0, szz = 0.0, sxz = 0.0, sxh = 0.0, szh = 0.0;
        for (const Mean &m : means)
        {
            const double dx = m.x - centre.x;
            const double dz = m.z - centre.z;
            const double dh = m.h - centre.h;
            sxx += dx * dx;
            szz += dz * dz;
            sxz += dx * dz;
            sxh += dx * dh;
            szh += dz * dh;
        }
        const double det = sxx * szz - sxz * sxz;
        const double a = det != 0.0 ? (sxh * szz - szh * sxz) / det : 0.0;
        const double b = det != 0.0 ? (szh * sxx - sxh * sxz) / det : 0.0;

        double low = 0.0, high = 0.0;
        for (size_t m = 0; m < means.size(); ++m)
        {
            const double r = means[m].h - (centre.h + a * (means[m].x - centre.x) + b * (means[m].z - centre.z));
            low = m == 0 ? r : std::min(low, r);
            high = m == 0 ? r : std::max(high, r);
        }
        return high - low;
    }

    // Opposite-sign second differences >= minCurvature: sample-scale detail.
    inline int CountCurvatureFlips(const MapGrid &grid, double minCurvature)
    {
        const auto flip = [&](double a, double b, double c, double d)
        {
            const double first = a - 2.0 * b + c;
            const double second = b - 2.0 * c + d;
            return std::abs(first) >= minCurvature && std::abs(second) >= minCurvature && (first < 0.0) != (second < 0.0);
        };
        int count = 0;
        for (size_t k = 0; k < grid.depth; ++k)
            for (size_t i = 0; i < grid.width; ++i)
            {
                if (i + 3 < grid.width && grid.In(i, k) && grid.In(i + 3, k))
                    count += flip(grid.At(i, k), grid.At(i + 1, k), grid.At(i + 2, k), grid.At(i + 3, k));
                if (k + 3 < grid.depth && grid.In(i, k) && grid.In(i, k + 3))
                    count += flip(grid.At(i, k), grid.At(i, k + 1), grid.At(i, k + 2), grid.At(i, k + 3));
            }
        return count;
    }
}
