/**
 * @file TerrainRegions.cpp
 * @author Jackey Cen
 * @brief Bakes a release's coarse Region hierarchy from its final heights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRegions.h"

#include "TerrainFinalSurface.h"

#include <terrain/TerrainLod.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            int64_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        double Cross(double ax, double az, double bx, double bz)
        {
            return ax * bz - az * bx;
        }

        struct RegionBuilder
        {
            const TerrainFinalSurface &final;
            const TerrainRegionGrid &grid;
            TerrainRegionCoord region;
            TerrainLocalBounds rect;
            TerrainRegionMesh mesh;
            std::map<std::pair<int64_t, int64_t>, uint32_t> ids;

            const TerrainLayout &Ref() const { return grid.Reference(); }

            // The tile holding a sample, the larger index on an edge, kept inside the map.
            const TileHeights &TileAt(int64_t ix, int64_t iz) const
            {
                const TileRange map = Ref().Tiles();
                const int64_t n = Ref().IntervalsPerTile();
                const int32_t tx = int32_t(std::clamp<int64_t>(FloorDiv(ix, n), map.min.x, map.max.x));
                const int32_t tz = int32_t(std::clamp<int64_t>(FloorDiv(iz, n), map.min.z, map.max.z));
                return *final.Tile({tx, tz});
            }

            int64_t Step(TileCoord tile) const
            {
                return Ref().IntervalsPerTile() / final.Tile(tile)->IntervalsPerTile();
            }

            uint32_t Vertex(int64_t ix, int64_t iz)
            {
                auto [it, inserted] = ids.try_emplace({iz, ix}, static_cast<uint32_t>(mesh.positions.size()));
                if (inserted)
                {
                    const double x = Ref().SamplePosition(ix), z = Ref().SamplePosition(iz);
                    const TileHeights &tile = TileAt(ix, iz);
                    mesh.positions.emplace_back(float(x - rect.minX), float(final.SharedHeight(x, z, tile)),
                                                float(z - rect.minZ));
                    mesh.normals.push_back(final.Normal(x, z, tile));
                }
                return it->second;
            }

            // Nodes strictly inside (from, to) on the edge at fixed: each tile segment at the finer step of
            // the two tiles beside it.
            std::vector<int64_t> EdgeNodes(int64_t fixed, int64_t from, int64_t to, bool vertical) const
            {
                const int64_t n = Ref().IntervalsPerTile();
                const int32_t before = int32_t(fixed / n - 1), after = int32_t(fixed / n);
                std::vector<int64_t> nodes;
                for (int64_t t = FloorDiv(from, n); t * n < to; ++t)
                {
                    const int32_t along = int32_t(t);
                    const TileCoord a = vertical ? TileCoord{before, along} : TileCoord{along, before};
                    const TileCoord b = vertical ? TileCoord{after, along} : TileCoord{along, after};
                    const int64_t step = std::min(Step(a), Step(b));
                    for (int64_t v = t * n; v <= (t + 1) * n; v += step)
                        if (v > from && v < to && (nodes.empty() || nodes.back() < v))
                            nodes.push_back(v);
                }
                return nodes;
            }
        };

        void Split(std::vector<std::array<uint32_t, 3>> &triangles, uint32_t start, uint32_t end,
                   const std::vector<uint32_t> &points)
        {
            uint32_t previous = start;
            for (uint32_t point : points)
            {
                for (size_t t = 0; t < triangles.size(); ++t)
                {
                    const std::array<uint32_t, 3> tri = triangles[t];
                    bool done = false;
                    for (int j = 0; j < 3; ++j)
                    {
                        const uint32_t u = tri[j], w = tri[(j + 1) % 3], third = tri[(j + 2) % 3];
                        if ((u == previous && w == end) || (u == end && w == previous))
                        {
                            triangles[t] = {u, point, third};
                            triangles.push_back({point, w, third});
                            done = true;
                            break;
                        }
                    }
                    if (done)
                        break;
                }
                previous = point;
            }
        }

        TerrainRegionMesh BuildRegion(const TerrainFinalSurface &final, const TerrainRegionGrid &grid,
                                      TerrainRegionCoord region)
        {
            RegionBuilder b{final, grid, region, grid.Rect(region), {}, {}};
            b.mesh.region = region;
            b.mesh.tiles = grid.MapTiles(region);
            b.mesh.cellSpacing = grid.CellSpacing(region.level);
            b.mesh.origin = *TerrainLocalPosition::FromMeters({b.rect.minX, 0.0, b.rect.minZ});

            const std::vector<int64_t> lx = grid.LinesX(region), lz = grid.LinesZ(region);
            const TileCoord count = grid.Count(region.level);
            // An edge with a neighbouring Region keeps every source node beside it.
            const bool left = region.x > 0, right = region.x < count.x - 1;
            const bool bottom = region.z > 0, top = region.z < count.z - 1;
            const std::vector<int64_t> leftNodes = left ? b.EdgeNodes(lx.front(), lz.front(), lz.back(), true)
                                                        : std::vector<int64_t>{};
            const std::vector<int64_t> rightNodes = right ? b.EdgeNodes(lx.back(), lz.front(), lz.back(), true)
                                                          : std::vector<int64_t>{};
            const std::vector<int64_t> bottomNodes = bottom ? b.EdgeNodes(lz.front(), lx.front(), lx.back(), false)
                                                            : std::vector<int64_t>{};
            const std::vector<int64_t> topNodes = top ? b.EdgeNodes(lz.back(), lx.front(), lx.back(), false)
                                                      : std::vector<int64_t>{};
            auto between = [](const std::vector<int64_t> &nodes, int64_t from, int64_t to) {
                const auto first = std::upper_bound(nodes.begin(), nodes.end(), from);
                const auto last = std::lower_bound(nodes.begin(), nodes.end(), to);
                return std::vector<int64_t>(first, last);
            };

            for (size_t j = 0; j + 1 < lz.size(); ++j)
                for (size_t i = 0; i + 1 < lx.size(); ++i)
                {
                    const int64_t x0 = lx[i], x1 = lx[i + 1], z0 = lz[j], z1 = lz[j + 1];
                    const uint32_t a = b.Vertex(x0, z0), bb = b.Vertex(x1, z0), c = b.Vertex(x1, z1),
                                   d = b.Vertex(x0, z1);
                    std::vector<std::array<uint32_t, 3>> triangles{{a, d, c}, {a, c, bb}};
                    auto points = [&](const std::vector<int64_t> &nodes, int64_t from, int64_t to, bool alongZ,
                                      int64_t fixed) {
                        std::vector<uint32_t> ids;
                        for (int64_t v : between(nodes, from, to))
                            ids.push_back(alongZ ? b.Vertex(fixed, v) : b.Vertex(v, fixed));
                        return ids;
                    };
                    if (left && i == 0)
                        Split(triangles, a, d, points(leftNodes, z0, z1, true, x0));
                    if (right && i + 2 == lx.size())
                        Split(triangles, bb, c, points(rightNodes, z0, z1, true, x1));
                    if (bottom && j == 0)
                        Split(triangles, a, bb, points(bottomNodes, x0, x1, false, z0));
                    if (top && j + 2 == lz.size())
                        Split(triangles, d, c, points(topNodes, x0, x1, false, z1));
                    for (const auto &tri : triangles)
                        b.mesh.indices.insert(b.mesh.indices.end(), tri.begin(), tri.end());
                }
            b.mesh.topologyHash = TerrainTopologyHash(b.mesh.positions, b.mesh.indices);
            return std::move(b.mesh);
        }
    }

    std::optional<TerrainRegionBake> BakeTerrainRegions(const TerrainRecipe &recipe,
                                                        std::span<const TileHeights> heights,
                                                        const TerrainRegionSettings &settings, std::string &error)
    {
        const TerrainLayout &ref = recipe.layout;
        TerrainFinalSurface final{recipe, {}};
        std::unordered_map<TileCoord, TerrainLayout> layouts;
        uint32_t unit = 1;
        for (const TileHeights &tile : heights)
        {
            const std::optional<TerrainLayout> layout = recipe.SourceLayout(tile.Tile());
            if (!layout || layout->IntervalsPerTile() != tile.IntervalsPerTile() ||
                ref.IntervalsPerTile() % layout->IntervalsPerTile() != 0 ||
                !final.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile).second)
            {
                error = std::format("tile ({}, {}) has no single source layout for its Region", tile.Tile().x,
                                    tile.Tile().z);
                return std::nullopt;
            }
            layouts.emplace(tile.Tile(), *layout);
            unit = std::max<uint32_t>(unit, uint32_t(ref.IntervalsPerTile() / layout->IntervalsPerTile()));
        }
        const TileRange map = ref.Tiles();
        std::vector<TerrainSourceChecksum> checksums;
        for (int32_t z = map.min.z; z <= map.max.z; ++z)
            for (int32_t x = map.min.x; x <= map.max.x; ++x)
            {
                const TileHeights *tile = final.Tile({x, z});
                if (!tile)
                {
                    error = std::format("tile ({}, {}) has no final heights for its Region", x, z);
                    return std::nullopt;
                }
                checksums.push_back({{x, z}, TerrainFinalHeightChecksum(*tile)});
            }
        const std::optional<TerrainRegionGrid> grid =
            TerrainRegionGrid::Create(ref, settings.regionTiles, settings.cells, unit);
        if (!grid)
        {
            error = std::format("a Region grid of {} tiles and {} cells does not fit this layout (unit {})",
                                settings.regionTiles, settings.cells, unit);
            return std::nullopt;
        }

        const TerrainSourceLookup sources = [&](TileCoord tile) {
            const auto it = layouts.find(tile);
            return it == layouts.end() ? TerrainSourceTile{} : TerrainSourceTile{&it->second, final.Tile(tile)};
        };
        TerrainRegionBake bake;
        bake.writes.grid = grid;
        std::vector<uint64_t> levelChecksums;
        for (uint64_t ordinal = 0; ordinal < grid->RegionCount(); ++ordinal)
        {
            const TerrainRegionCoord region = grid->AtOrdinal(ordinal);
            if (region.x == 0 && region.z == 0)
                levelChecksums = TerrainRegionSourceChecksums(*grid, region.level, checksums);
            TerrainRegionMesh mesh = BuildRegion(final, *grid, region);
            mesh.sourceChecksum = levelChecksums[size_t(region.z) * size_t(grid->Count(region.level).x) +
                                                 size_t(region.x)];
            const std::string name = std::format("Region L{} ({}, {})", region.level, region.x, region.z);
            if (mesh.positions.size() > UINT32_MAX || mesh.indices.size() > UINT32_MAX ||
                !TerrainRegionCoversDomain(*grid, mesh))
            {
                error = name + ": the mesh does not cover its domain";
                return std::nullopt;
            }
            const std::optional<double> deviation = TerrainRegionDeviation(*grid, mesh, sources);
            if (!deviation)
            {
                error = name + ": the error against the final surface cannot be enclosed";
                return std::nullopt;
            }
            mesh.maxDeviation = *deviation;

            TerrainTileRecord record;
            record.tile = {region.x, region.z};
            record.level = region.level;
            record.layer = 0;
            record.kind = TerrainPayloadKind::Region;
            record.source = false;
            record.samplesPerSide = grid->SamplesPerSide(region.level);
            record.sampleSpacing = mesh.cellSpacing;
            const auto [lo, hi] = std::minmax_element(mesh.positions.begin(), mesh.positions.end(),
                                                      [](const glm::vec3 &a, const glm::vec3 &b) { return a.y < b.y; });
            record.minHeight = lo->y;
            record.maxHeight = hi->y;
            record.validExtent = grid->ValidExtent(region);
            record.maxDeviation = mesh.maxDeviation;
            record.sourceChecksum = mesh.sourceChecksum;
            record.topologyHash = mesh.topologyHash;
            record.vertexCount = static_cast<uint32_t>(mesh.positions.size());
            record.indexCount = static_cast<uint32_t>(mesh.indices.size());
            bake.writes.payloads.push_back({record, EncodeTerrainRegion(mesh)});
            bake.meshes.push_back(std::move(mesh));
        }
        return bake;
    }

    TerrainRegionSampler::TerrainRegionSampler(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh)
        : mMesh(mesh)
    {
        const TerrainLayout &ref = grid.Reference();
        const TerrainLocalBounds rect = grid.Rect(mesh.region);
        for (int64_t line : grid.LinesX(mesh.region))
            mLinesX.push_back(ref.SamplePosition(line) - rect.minX);
        for (int64_t line : grid.LinesZ(mesh.region))
            mLinesZ.push_back(ref.SamplePosition(line) - rect.minZ);
        mCells.resize((mLinesX.size() - 1) * (mLinesZ.size() - 1));
        auto cell = [&](const std::vector<double> &lines, double v) {
            const auto it = std::upper_bound(lines.begin() + 1, lines.end() - 1, v);
            return size_t(it - lines.begin() - 1);
        };
        for (uint32_t t = 0; t + 2 < mesh.indices.size(); t += 3)
        {
            double cx = 0.0, cz = 0.0;
            for (uint32_t j = 0; j < 3; ++j)
            {
                cx += mesh.positions[mesh.indices[t + j]].x / 3.0;
                cz += mesh.positions[mesh.indices[t + j]].z / 3.0;
            }
            mCells[cell(mLinesZ, cz) * (mLinesX.size() - 1) + cell(mLinesX, cx)].push_back(t);
        }
    }

    std::optional<double> TerrainRegionSampler::HeightAt(double x, double z) const
    {
        if (!std::isfinite(x) || !std::isfinite(z) || x < mLinesX.front() || x > mLinesX.back() ||
            z < mLinesZ.front() || z > mLinesZ.back())
            return std::nullopt;
        const size_t cx = size_t(std::upper_bound(mLinesX.begin() + 1, mLinesX.end() - 1, x) - mLinesX.begin() - 1);
        const size_t cz = size_t(std::upper_bound(mLinesZ.begin() + 1, mLinesZ.end() - 1, z) - mLinesZ.begin() - 1);
        for (uint32_t t : mCells[cz * (mLinesX.size() - 1) + cx])
        {
            std::array<glm::dvec3, 3> v;
            for (uint32_t j = 0; j < 3; ++j)
                v[j] = glm::dvec3(mMesh.positions[mMesh.indices[t + j]]);
            bool inside = true;
            for (size_t j = 0; j < 3 && inside; ++j)
            {
                const glm::dvec3 a = v[j], b = v[(j + 1) % 3];
                const double side = Cross(b.x - a.x, b.z - a.z, x - a.x, z - a.z);
                const double scale = std::abs((b.x - a.x) * (z - a.z)) + std::abs((b.z - a.z) * (x - a.x));
                inside = side <= 64 * std::numeric_limits<double>::epsilon() * scale;
            }
            if (!inside)
                continue;
            const double d = Cross(v[1].x - v[0].x, v[1].z - v[0].z, v[2].x - v[0].x, v[2].z - v[0].z);
            const double u = Cross(x - v[0].x, z - v[0].z, v[2].x - v[0].x, v[2].z - v[0].z) / d;
            const double w = Cross(v[1].x - v[0].x, v[1].z - v[0].z, x - v[0].x, z - v[0].z) / d;
            return v[0].y + u * (v[1].y - v[0].y) + w * (v[2].y - v[0].y);
        }
        return std::nullopt;
    }
}
