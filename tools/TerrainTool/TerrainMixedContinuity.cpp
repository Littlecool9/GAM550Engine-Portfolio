#include "TerrainContinuity.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace mir::terrainbake
{
    double TerrainSeamTolerance(double supportMagnitude, double coordinateMagnitude, double minSpacing)
    {
        const double m = std::max(1.0, supportMagnitude);
        return SurfaceTolerance(m) + 32.0 * std::numeric_limits<double>::epsilon() * m *
            (1.0 + coordinateMagnitude / minSpacing);
    }

    namespace
    {
        bool Valid(const TerrainContinuityTile &v)
        {
            if (!v.layout) return false;
            const size_t n = size_t(v.layout->IntervalsPerTile()) + 1;
            return v.samples.size() == n * n;
        }
        float At(const TerrainContinuityTile &v, int32_t i, int32_t k)
        {
            return v.samples[size_t(k) * (v.layout->IntervalsPerTile() + 1) + i];
        }
        std::vector<double> Positions(const TerrainLayout &layout, int32_t alongTile)
        {
            std::vector<double> out;
            const int64_t first = layout.TileFirstSample(alongTile);
            for (int32_t a = 0; a <= layout.IntervalsPerTile(); ++a) out.push_back(layout.SamplePosition(first + a));
            return out;
        }
        GlobalSampleIndex Cell(const TerrainLayout &layout, TileCoord tile, bool acrossX, bool highSide,
                               const std::vector<double> &positions, double along)
        {
            const auto upper = std::upper_bound(positions.begin(), positions.end(), along);
            const int64_t a = std::clamp<int64_t>(upper - positions.begin() - 1, 0, layout.IntervalsPerTile() - 1);
            const int64_t across = layout.TileFirstSample(acrossX ? tile.x : tile.z) +
                (highSide ? 0 : layout.IntervalsPerTile() - 1);
            const int64_t coordinate = layout.TileFirstSample(acrossX ? tile.z : tile.x) + a;
            return acrossX ? GlobalSampleIndex{across, coordinate} : GlobalSampleIndex{coordinate, across};
        }
        double Magnitude(const TerrainContinuityTile &v, TileCoord tile, GlobalSampleIndex cell)
        {
            const int32_t i = int32_t(cell.x - v.layout->TileFirstSample(tile.x));
            const int32_t k = int32_t(cell.z - v.layout->TileFirstSample(tile.z));
            return std::max({std::abs(double(At(v, i, k))), std::abs(double(At(v, i + 1, k))),
                std::abs(double(At(v, i, k + 1))), std::abs(double(At(v, i + 1, k + 1)))});
        }
    }

    ContinuityStats CheckMixedTileContinuity(const TerrainLayout &reference, const TerrainContinuityLookup &lookup,
                                            TerrainReport &report)
    {
        ContinuityStats stats;
        const auto range = reference.Tiles();
        const auto bounds = reference.Bounds();
        auto edge = [&](TileCoord lo, TileCoord hi, bool acrossX) {
            ++stats.edges;
            const auto a = lookup(lo), b = lookup(hi);
            if (!Valid(a) || !Valid(b)) return;
            ++stats.edgesCompared;
            const double across = reference.SamplePosition(reference.TileFirstSample(acrossX ? hi.x : hi.z));
            const auto pa = Positions(*a.layout, acrossX ? lo.z : lo.x);
            const auto pb = Positions(*b.layout, acrossX ? hi.z : hi.x);
            const double begin = std::max(pa.front(), acrossX ? bounds.minZ : bounds.minX);
            const double end = std::min(pa.back(), acrossX ? bounds.maxZ : bounds.maxX);
            std::vector<double> knots{begin, end};
            for (double p : pa) if (p > begin && p < end) knots.push_back(p);
            for (double p : pb) if (p > begin && p < end) knots.push_back(p);
            std::sort(knots.begin(), knots.end());
            knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
            stats.breakpoints += knots.size();
            uint64_t failed = 0;
            double largest = 0.0, firstX = 0.0, firstZ = 0.0, firstTolerance = 0.0;
            auto probe = [&](double along) {
                const double x = acrossX ? across : along, z = acrossX ? along : across;
                const auto local = TerrainLocalPosition::FromMeters({x, 0.0, z});
                const auto ca = Cell(*a.layout, lo, acrossX, false, pa, along);
                const auto cb = Cell(*b.layout, hi, acrossX, true, pb, along);
                const auto sa = EvaluateSurfaceInCell(*a.layout, {lo, a.samples}, ca, *local);
                const auto sb = EvaluateSurfaceInCell(*b.layout, {hi, b.samples}, cb, *local);
                ++stats.probes;
                const double support = std::max(Magnitude(a, lo, ca), Magnitude(b, hi, cb));
                const double tolerance = TerrainSeamTolerance(support, std::max(std::abs(x), std::abs(z)),
                    std::min(a.layout->SampleSpacing(), b.layout->SampleSpacing()));
                stats.maxTolerance = std::max(stats.maxTolerance, tolerance);
                const double difference = std::abs(sa.height - sb.height);
                stats.maxSurfaceDifference = std::max(stats.maxSurfaceDifference, difference);
                if (!sa.IsOk() || !sb.IsOk() || !std::isfinite(difference) || difference > tolerance)
                {
                    if (failed++ == 0) { firstX = x; firstZ = z; firstTolerance = tolerance; }
                    largest = std::max(largest, difference);
                }
            };
            for (size_t k = 0; k < knots.size(); ++k)
            {
                probe(knots[k]);
                if (k + 1 < knots.size())
                    for (int p = 1; p < kEdgeProbesPerCell; ++p)
                        probe(knots[k] + (knots[k + 1] - knots[k]) * p / kEdgeProbesPerCell);
            }
            // Common vertices must keep the exact stored value, including signed zero.
            bool bitMismatch = false;
            for (size_t i = 0; i < pa.size(); ++i)
            {
                if (pa[i] < begin || pa[i] > end) continue;
                const auto it = std::lower_bound(pb.begin(), pb.end(), pa[i]);
                if (it == pb.end() || *it != pa[i]) continue;
                const int32_t j = int32_t(it - pb.begin());
                const float va = acrossX ? At(a, a.layout->IntervalsPerTile(), int32_t(i)) : At(a, int32_t(i), a.layout->IntervalsPerTile());
                const float vb = acrossX ? At(b, 0, j) : At(b, j, 0);
                if (std::bit_cast<uint32_t>(va) != std::bit_cast<uint32_t>(vb)) bitMismatch = true;
            }
            if (bitMismatch) report.Add({std::string(check::kEdges), "samples", {lo, hi}, std::nullopt,
                "common edge vertices are not bit-identical"});
            if (failed) report.Add({std::string(check::kEdges), "surface", {lo, hi}, std::nullopt,
                std::format("{} edge probes disagree; first ({}, {}) m, tolerance {} m, largest difference {} m",
                    failed, firstX, firstZ, firstTolerance, largest)});
        };
        for (int64_t z = range.min.z; z <= range.max.z; ++z)
            for (int64_t x = range.min.x; x <= range.max.x; ++x)
            {
                const TileCoord lo{int32_t(x), int32_t(z)};
                if (x < range.max.x) edge(lo, {int32_t(x + 1), int32_t(z)}, true);
                if (z < range.max.z) edge(lo, {int32_t(x), int32_t(z + 1)}, false);
                if (x >= range.max.x || z >= range.max.z) continue;
                ++stats.corners;
                const TileCoord tiles[4] = {lo, {int32_t(x + 1), int32_t(z)}, {int32_t(x), int32_t(z + 1)},
                    {int32_t(x + 1), int32_t(z + 1)}};
                TerrainContinuityTile views[4];
                bool valid = true;
                for (int t = 0; t < 4; ++t) { views[t] = lookup(tiles[t]); valid &= Valid(views[t]); }
                if (!valid) continue;
                ++stats.cornersCompared;
                const float heights[4] = {At(views[0], views[0].layout->IntervalsPerTile(), views[0].layout->IntervalsPerTile()),
                    At(views[1], 0, views[1].layout->IntervalsPerTile()), At(views[2], views[2].layout->IntervalsPerTile(), 0), At(views[3], 0, 0)};
                if (std::any_of(heights + 1, heights + 4, [&](float h) { return std::bit_cast<uint32_t>(h) != std::bit_cast<uint32_t>(heights[0]); }))
                    report.Add({std::string(check::kCorners), "samples", {tiles, tiles + 4},
                        GlobalSampleIndex{reference.TileFirstSample(int32_t(x + 1)), reference.TileFirstSample(int32_t(z + 1))},
                        "mixed four-tile corner values are not bit-identical"});
            }
        auto finish = [&](std::string_view name, uint32_t compared, uint32_t total) {
            const auto detail = std::format("{} of {} compared; {} union breakpoints and {} dense probes, max difference {} m, max tolerance {} m",
                compared, total, stats.breakpoints, stats.probes, stats.maxSurfaceDifference, stats.maxTolerance);
            if (compared == total) report.Ran(name, detail); else report.Skip(name, detail + " (unreadable source tile)");
        };
        finish(check::kEdges, stats.edgesCompared, stats.edges);
        finish(check::kCorners, stats.cornersCompared, stats.corners);
        return stats;
    }
}
