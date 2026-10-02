/**
 * @file TerrainContinuity.cpp
 * @author Jackey Cen
 * @brief Seam checks between neighbouring tiles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainContinuity.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        std::string Tile(TileCoord t)
        {
            return std::format("({}, {})", t.x, t.z);
        }

        // +0 and -0 differ.
        bool SameBits(float a, float b)
        {
            return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
        }

        bool InBounds(const TerrainLocalBounds &b, double x, double z)
        {
            return x >= b.minX && x <= b.maxX && z >= b.minZ && z <= b.maxZ;
        }

        struct SideEval
        {
            TileCoord tile;
            std::span<const float> samples;
            GlobalSampleIndex cell;
        };

        struct Probe
        {
            bool ok = true;
            SurfaceStatus status = SurfaceStatus::Ok;
            double low = 0.0;
            double high = 0.0;
        };

        Probe EvaluateSides(const TerrainLayout &layout, std::span<const SideEval> sides, double x, double z)
        {
            Probe probe;
            const TerrainLocalPosition local = *TerrainLocalPosition::FromMeters({x, 0.0, z});
            for (size_t s = 0; s < sides.size(); ++s)
            {
                const SurfaceSample sample =
                    EvaluateSurfaceInCell(layout, {sides[s].tile, sides[s].samples}, sides[s].cell, local);
                if (!sample.IsOk())
                {
                    probe.ok = false;
                    probe.status = sample.status;
                    return probe;
                }
                probe.low = s == 0 ? sample.height : std::min(probe.low, sample.height);
                probe.high = s == 0 ? sample.height : std::max(probe.high, sample.height);
            }
            return probe;
        }

        std::string_view StatusName(SurfaceStatus status)
        {
            switch (status)
            {
            case SurfaceStatus::Ok:
                return "Ok";
            case SurfaceStatus::OutsideMap:
                return "OutsideMap";
            case SurfaceStatus::InvalidInput:
                return "InvalidInput";
            case SurfaceStatus::MissingTile:
                return "MissingTile";
            case SurfaceStatus::InvalidSamples:
                return "InvalidSamples";
            }
            return "?";
        }

        class ContinuityChecker
        {
        public:
            ContinuityChecker(const TerrainLayout &layout, const TileSampleLookup &lookup, TerrainReport &report)
                : mLayout(layout), mLookup(lookup), mReport(report), mN(layout.IntervalsPerTile())
            {
            }

            ContinuityStats Run()
            {
                const TileRange range = mLayout.Tiles();
                for (int32_t z = range.min.z; z <= range.max.z; ++z)
                    for (int32_t x = range.min.x; x <= range.max.x; ++x)
                    {
                        if (x < range.max.x)
                            Edge({x, z}, {x + 1, z}, true);
                        if (z < range.max.z)
                            Edge({x, z}, {x, z + 1}, false);
                        if (x < range.max.x && z < range.max.z)
                            Corner(x, z);
                    }
                return mStats;
            }

        private:
            std::span<const float> Samples(TileCoord tile) const
            {
                const size_t side = static_cast<size_t>(mN) + 1;
                std::span<const float> s = mLookup ? mLookup(tile) : std::span<const float>();
                return s.size() == side * side ? s : std::span<const float>();
            }

            float At(std::span<const float> s, int32_t i, int32_t k) const
            {
                return s[static_cast<size_t>(k) * (mN + 1) + static_cast<size_t>(i)];
            }

            void Edge(TileCoord lo, TileCoord hi, bool acrossX)
            {
                ++mStats.edges;
                const std::span<const float> a = Samples(lo);
                const std::span<const float> b = Samples(hi);
                if (a.empty() || b.empty())
                    return;
                ++mStats.edgesCompared;

                const int64_t edge = acrossX ? mLayout.TileFirstSample(hi.x) : mLayout.TileFirstSample(hi.z);
                const int64_t along0 = acrossX ? mLayout.TileFirstSample(lo.z) : mLayout.TileFirstSample(lo.x);
                auto global = [&](int64_t across, int64_t along) {
                    return acrossX ? GlobalSampleIndex{across, along} : GlobalSampleIndex{along, across};
                };
                const std::string where = std::format("the edge {} = {} m between tiles {} and {}",
                                                      acrossX ? "x" : "z", mLayout.SamplePosition(edge), Tile(lo),
                                                      Tile(hi));

                uint32_t differ = 0;
                int32_t first = 0;
                double largest = 0.0;
                for (int32_t t = 0; t <= mN; ++t)
                {
                    const float va = acrossX ? At(a, mN, t) : At(a, t, mN);
                    const float vb = acrossX ? At(b, 0, t) : At(b, t, 0);
                    if (SameBits(va, vb))
                        continue;
                    if (differ++ == 0)
                        first = t;
                    largest = std::max(largest, std::abs(double(va) - double(vb)));
                }
                if (differ > 0)
                {
                    const float va = acrossX ? At(a, mN, first) : At(a, first, mN);
                    const float vb = acrossX ? At(b, 0, first) : At(b, first, 0);
                    mReport.Add({std::string(check::kEdges), "samples", {lo, hi}, global(edge, along0 + first),
                                 std::format("{} of {} shared samples on {} differ; first {} in tile {} vs {} in "
                                             "tile {}, largest difference {} m",
                                             differ, mN + 1, where, va, Tile(lo), vb, Tile(hi), largest)});
                }

                const double s = mLayout.SampleSpacing();
                const double across = mLayout.SamplePosition(edge);
                uint32_t bad = 0;
                std::string firstBad;
                double worst = 0.0;
                for (int32_t c = 0; c < mN; ++c)
                {
                    const int64_t cellAlong = along0 + c;
                    const SideEval sides[2] = {{lo, a, global(edge - 1, cellAlong)}, {hi, b, global(edge, cellAlong)}};
                    for (int32_t p = 0; p <= kEdgeProbesPerCell; ++p)
                    {
                        // Only the last cell probes its far end.
                        if (p == kEdgeProbesPerCell && c + 1 < mN)
                            continue;
                        const double along = p == kEdgeProbesPerCell
                                                 ? mLayout.SamplePosition(cellAlong + 1)
                                                 : mLayout.SamplePosition(cellAlong) + s * p / kEdgeProbesPerCell;
                        const double x = acrossX ? across : along;
                        const double z = acrossX ? along : across;
                        if (!InBounds(mLayout.Bounds(), x, z))
                            continue;
                        ++mStats.probes;
                        const Probe probe = EvaluateSides(mLayout, sides, x, z);
                        if (!probe.ok)
                        {
                            if (bad++ == 0)
                                firstBad = std::format("({}, {}) m evaluates to {}", x, z, StatusName(probe.status));
                            continue;
                        }
                        const double diff = probe.high - probe.low;
                        mStats.maxSurfaceDifference = std::max(mStats.maxSurfaceDifference, diff);
                        if (diff <= SurfaceTolerance(std::max(std::abs(probe.low), std::abs(probe.high))))
                            continue;
                        if (bad++ == 0)
                            firstBad = std::format("({}, {}) m spans [{}, {}] m", x, z, probe.low, probe.high);
                        worst = std::max(worst, diff);
                    }
                }
                if (bad > 0)
                    mReport.Add({std::string(check::kEdges), "surface", {lo, hi}, std::nullopt,
                                 std::format("the surface from the two sides of {} disagrees beyond SurfaceTolerance "
                                             "at {} probes; first at {}, largest difference {} m",
                                             where, bad, firstBad, worst)});
            }

            void Corner(int32_t x, int32_t z)
            {
                ++mStats.corners;
                const TileCoord tiles[4] = {{x, z}, {x + 1, z}, {x, z + 1}, {x + 1, z + 1}};
                std::span<const float> s[4];
                for (int t = 0; t < 4; ++t)
                {
                    s[t] = Samples(tiles[t]);
                    if (s[t].empty())
                        return;
                }
                ++mStats.cornersCompared;

                const int64_t ci = mLayout.TileFirstSample(x + 1);
                const int64_t ck = mLayout.TileFirstSample(z + 1);
                const float values[4] = {At(s[0], mN, mN), At(s[1], 0, mN), At(s[2], mN, 0), At(s[3], 0, 0)};
                const std::vector<TileCoord> named(std::begin(tiles), std::end(tiles));
                const double px = mLayout.SamplePosition(ci);
                const double pz = mLayout.SamplePosition(ck);
                if (!SameBits(values[0], values[1]) || !SameBits(values[0], values[2]) ||
                    !SameBits(values[0], values[3]))
                    mReport.Add({std::string(check::kCorners), "samples", named, GlobalSampleIndex{ci, ck},
                                 std::format("the four-tile corner ({}, {}) m holds {} in {}, {} in {}, {} in {} and {} "
                                             "in {}",
                                             px, pz, values[0], Tile(tiles[0]), values[1], Tile(tiles[1]),
                                             values[2], Tile(tiles[2]), values[3], Tile(tiles[3]))});

                const SideEval sides[4] = {{tiles[0], s[0], {ci - 1, ck - 1}},
                                           {tiles[1], s[1], {ci, ck - 1}},
                                           {tiles[2], s[2], {ci - 1, ck}},
                                           {tiles[3], s[3], {ci, ck}}};
                ++mStats.probes;
                const Probe probe = EvaluateSides(mLayout, sides, px, pz);
                if (!probe.ok)
                {
                    mReport.Add({std::string(check::kCorners), "surface", named, GlobalSampleIndex{ci, ck},
                                 std::format("the four-tile corner ({}, {}) m evaluates to {}", px, pz,
                                             StatusName(probe.status))});
                    return;
                }
                const double diff = probe.high - probe.low;
                mStats.maxSurfaceDifference = std::max(mStats.maxSurfaceDifference, diff);
                if (diff > SurfaceTolerance(std::max(std::abs(probe.low), std::abs(probe.high))))
                    mReport.Add({std::string(check::kCorners), "surface", named, GlobalSampleIndex{ci, ck},
                                 std::format("the surface at the four-tile corner ({}, {}) m spans [{}, {}] m across "
                                             "its four cells, beyond SurfaceTolerance",
                                             px, pz, probe.low, probe.high)});
            }

            const TerrainLayout &mLayout;
            const TileSampleLookup &mLookup;
            TerrainReport &mReport;
            int32_t mN;
            ContinuityStats mStats;
        };
    }

    ContinuityStats CheckTileContinuity(const TerrainLayout &layout, const TileSampleLookup &lookup,
                                        TerrainReport &report)
    {
        const ContinuityStats stats = ContinuityChecker(layout, lookup, report).Run();
        auto finish = [&](std::string_view name, uint32_t compared, uint32_t total, const char *what) {
            std::string detail = std::format("{} of {} {} compared", compared, total, what);
            if (name == check::kEdges)
                detail += std::format(", {} surface probes, largest difference {} m", stats.probes,
                                      stats.maxSurfaceDifference);
            if (compared < total)
                report.Skip(name, detail + " (a tile is unreadable)");
            else
                report.Ran(name, detail);
        };
        finish(check::kEdges, stats.edgesCompared, stats.edges, "shared edges");
        finish(check::kCorners, stats.cornersCompared, stats.corners, "four-tile corners");
        return stats;
    }
}
