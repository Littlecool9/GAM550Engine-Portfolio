/**
 * @file TerrainRegion.cpp
 * @author Jackey Cen
 * @brief Coarse Region mesh payloads: encoding, checked decoding, coverage and error against the final surface.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainRegion.h"

#include "LittleEndian.h"
#include "PackInternal.h"
#include "TerrainOverlay.h"

#include <assets/AssetId.h>
#include <terrain/TerrainLod.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Fail;
        using detail::LoadF32;
        using detail::LoadF64;
        using detail::LoadI32;
        using detail::LoadU32;
        using detail::LoadU64;

        constexpr uint32_t kRegionMagic = 0x314e4752;

        std::string RegionName(TerrainRegionCoord region)
        {
            return std::format("Region L{} ({}, {})", region.level, region.x, region.z);
        }

        double Cross(double ax, double az, double bx, double bz)
        {
            return ax * bz - az * bx;
        }

        // Plane through the triangle at (x, z).
        double PlaneHeight(const std::array<glm::dvec3, 3> &t, double x, double z)
        {
            const double d = Cross(t[1].x - t[0].x, t[1].z - t[0].z, t[2].x - t[0].x, t[2].z - t[0].z);
            const double u = Cross(x - t[0].x, z - t[0].z, t[2].x - t[0].x, t[2].z - t[0].z) / d;
            const double v = Cross(t[1].x - t[0].x, t[1].z - t[0].z, x - t[0].x, z - t[0].z) / d;
            return t[0].y + u * (t[1].y - t[0].y) + v * (t[2].y - t[0].y);
        }

        struct FloatDomain
        {
            float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        };

        // The domain as the builder's float positions round it.
        FloatDomain DomainRelative(const TerrainRegionGrid &grid, TerrainRegionCoord region)
        {
            const TerrainLayout &ref = grid.Reference();
            const SampleRange domain = grid.Domain(region);
            const TerrainLocalBounds rect = grid.Rect(region);
            return {float(ref.SamplePosition(domain.min.x) - rect.minX), float(ref.SamplePosition(domain.min.z) - rect.minZ),
                    float(ref.SamplePosition(domain.max.x) - rect.minX), float(ref.SamplePosition(domain.max.z) - rect.minZ)};
        }

        // Source node indices of a tile-local coordinate, when it is one exactly.
        std::optional<int64_t> NodeOf(const TerrainLayout &layout, int64_t first, double origin, double coordinate)
        {
            const int64_t i = std::llround(coordinate / layout.SampleSpacing());
            if (layout.SamplePosition(first + i) - origin != coordinate)
                return std::nullopt;
            return i;
        }
    }

    std::vector<uint64_t> TerrainRegionSourceChecksums(const TerrainRegionGrid &grid, uint32_t level,
                                                       std::span<const TerrainSourceChecksum> sources)
    {
        const TileCoord count = grid.Count(level);
        std::vector<std::vector<std::byte>> bytes(size_t(count.x) * size_t(count.z));
        for (const TerrainSourceChecksum &source : sources)
        {
            const TerrainRegionCoord region = grid.Containing(level, source.tile);
            if (!grid.Contains(region))
                continue;
            detail::LeWriter out(bytes[size_t(region.z) * size_t(count.x) + size_t(region.x)]);
            out.I32(source.tile.x);
            out.I32(source.tile.z);
            out.U64(source.checksum);
        }
        std::vector<uint64_t> hashes;
        hashes.reserve(bytes.size());
        for (const std::vector<std::byte> &b : bytes)
            hashes.push_back(Fnv1a64(b));
        return hashes;
    }

    TerrainResult<std::vector<TerrainPayloadWrite>> detail::OrderRegionWrites(const TerrainRegionWrites &regions,
                                                                              const TerrainLayout &reference)
    {
        using R = std::vector<TerrainPayloadWrite>;
        R ordered(regions.payloads.begin(), regions.payloads.end());
        if (!regions.grid)
        {
            if (!ordered.empty())
                return Fail<R>(Code::InvalidInput, "Region payloads need a Region grid");
            return {std::move(ordered), {}};
        }
        const TerrainRegionGrid &grid = *regions.grid;
        if (grid.Reference().SampleSpacing() != reference.SampleSpacing() ||
            grid.Reference().IntervalsPerTile() != reference.IntervalsPerTile() ||
            grid.Reference().Bounds() != reference.Bounds())
            return Fail<R>(Code::InvalidInput, "the Region grid is not over the reference layer");
        if (ordered.size() != grid.RegionCount() || ordered.size() > UINT32_MAX)
            return Fail<R>(Code::InvalidInput, "{} Region payloads for a grid of {} Regions", ordered.size(),
                           grid.RegionCount());
        for (const TerrainPayloadWrite &v : ordered)
            if (!grid.Contains({v.record.level, v.record.tile.x, v.record.tile.z}) ||
                v.record.kind != TerrainPayloadKind::Region)
                return Fail<R>(Code::InvalidInput, "a Region payload names no Region of the grid");
        std::sort(ordered.begin(), ordered.end(), [&](const TerrainPayloadWrite &a, const TerrainPayloadWrite &b) {
            return grid.Ordinal({a.record.level, a.record.tile.x, a.record.tile.z}) <
                   grid.Ordinal({b.record.level, b.record.tile.x, b.record.tile.z});
        });
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            TerrainTileRecord &r = ordered[i].record;
            if (grid.Ordinal({r.level, r.tile.x, r.tile.z}) != i)
                return Fail<R>(Code::InvalidInput, "Region payloads must name every Region once; {} repeats",
                               RegionName({r.level, r.tile.x, r.tile.z}));
            r.length = ordered[i].bytes.size();
            r.checksum = Fnv1a64(ordered[i].bytes);
        }
        return {std::move(ordered), {}};
    }

    uint64_t TerrainRegionPayloadLength(uint32_t vertexCount, uint32_t indexCount)
    {
        return uint64_t(kTerrainRegionHeaderSize) + uint64_t(vertexCount) * 24 + uint64_t(indexCount) * 4;
    }

    std::vector<std::byte> EncodeTerrainRegion(const TerrainRegionMesh &mesh)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(size_t(TerrainRegionPayloadLength(uint32_t(mesh.positions.size()), uint32_t(mesh.indices.size()))));
        detail::LeWriter out(bytes);
        out.U32(kRegionMagic);
        out.U32(kTerrainRegionContentVersion);
        out.U32(mesh.region.level);
        out.I32(mesh.region.x);
        out.I32(mesh.region.z);
        out.U32(0);
        out.F64(mesh.origin.Meters().x);
        out.F64(mesh.origin.Meters().z);
        out.F64(mesh.cellSpacing);
        out.I32(mesh.tiles.min.x);
        out.I32(mesh.tiles.min.z);
        out.I32(mesh.tiles.max.x);
        out.I32(mesh.tiles.max.z);
        out.U64(mesh.sourceChecksum);
        out.U64(mesh.topologyHash);
        out.U32(static_cast<uint32_t>(mesh.positions.size()));
        out.U32(static_cast<uint32_t>(mesh.indices.size()));
        out.F64(mesh.maxDeviation);
        out.U64(0);
        out.U64(0);
        for (const glm::vec3 &p : mesh.positions)
        {
            out.F32(p.x);
            out.F32(p.y);
            out.F32(p.z);
        }
        for (const glm::vec3 &n : mesh.normals)
        {
            out.F32(n.x);
            out.F32(n.y);
            out.F32(n.z);
        }
        for (uint32_t i : mesh.indices)
            out.U32(i);
        return bytes;
    }

    TerrainResult<TerrainRegionMesh> DecodeTerrainRegion(const TerrainIndex &index, const TerrainTileRecord &record,
                                                         std::span<const std::byte> bytes)
    {
        using R = TerrainRegionMesh;
        const TerrainRegionCoord region{record.level, record.tile.x, record.tile.z};
        const std::string name = RegionName(region);
        if (record.kind != TerrainPayloadKind::Region || !index.regionGrid || !index.regionGrid->Contains(region))
            return Fail<R>(Code::InvalidInput, "{}: not a Region record of this index", name);
        const TerrainRegionGrid &grid = *index.regionGrid;
        if (bytes.size() != record.length || bytes.size() < kTerrainRegionHeaderSize)
            return Fail<R>(Code::Corrupt, "{}: payload is {} bytes, the index says {}", name, bytes.size(),
                           record.length);
        if (Fnv1a64(bytes) != record.checksum)
            return Fail<R>(Code::Corrupt, "{}: payload checksum differs from the index", name);
        if (LoadU32(bytes, 0) != kRegionMagic || LoadU32(bytes, 4) != kTerrainRegionContentVersion)
            return Fail<R>(Code::Unsupported, "{}: payload magic or content version {} is not supported", name,
                           LoadU32(bytes, 4));

        const TerrainLocalBounds rect = grid.Rect(region);
        const TileRange tiles = grid.MapTiles(region);
        const uint32_t vertexCount = LoadU32(bytes, 80), indexCount = LoadU32(bytes, 84);
        if (LoadU32(bytes, 8) != region.level || LoadI32(bytes, 12) != region.x || LoadI32(bytes, 16) != region.z ||
            LoadU32(bytes, 20) != 0 || LoadF64(bytes, 24) != rect.minX || LoadF64(bytes, 32) != rect.minZ ||
            LoadF64(bytes, 40) != record.sampleSpacing || LoadI32(bytes, 48) != tiles.min.x ||
            LoadI32(bytes, 52) != tiles.min.z || LoadI32(bytes, 56) != tiles.max.x || LoadI32(bytes, 60) != tiles.max.z ||
            LoadU64(bytes, 64) != record.sourceChecksum || LoadU64(bytes, 72) != record.topologyHash ||
            vertexCount != record.vertexCount || indexCount != record.indexCount || !record.maxDeviation ||
            LoadF64(bytes, 88) != *record.maxDeviation || LoadU64(bytes, 96) != 0 || LoadU64(bytes, 104) != 0)
            return Fail<R>(Code::Corrupt, "{}: payload header differs from its index record and grid", name);
        if (vertexCount < 3 || indexCount < 3 || indexCount % 3 != 0 ||
            TerrainRegionPayloadLength(vertexCount, indexCount) != bytes.size())
            return Fail<R>(Code::Corrupt, "{}: vertex and index counts do not fill the payload", name);

        R mesh;
        mesh.release = index.release;
        mesh.region = region;
        mesh.tiles = tiles;
        mesh.cellSpacing = record.sampleSpacing;
        mesh.sourceChecksum = record.sourceChecksum;
        mesh.topologyHash = record.topologyHash;
        mesh.maxDeviation = *record.maxDeviation;
        const std::optional<TerrainLocalPosition> origin = TerrainLocalPosition::FromMeters({rect.minX, 0.0, rect.minZ});
        if (!origin)
            return Fail<R>(Code::Corrupt, "{}: origin is not finite", name);
        mesh.origin = *origin;

        const FloatDomain domain = DomainRelative(grid, region);
        float lo = std::numeric_limits<float>::infinity(), hi = -lo;
        mesh.positions.reserve(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const size_t at = kTerrainRegionHeaderSize + size_t(i) * 12;
            const glm::vec3 p(LoadF32(bytes, at), LoadF32(bytes, at + 4), LoadF32(bytes, at + 8));
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.x < domain.minX ||
                p.x > domain.maxX || p.z < domain.minZ || p.z > domain.maxZ)
                return Fail<R>(Code::Corrupt, "{}: vertex {} is not finite or lies outside the Region's domain", name,
                               i);
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
            mesh.positions.push_back(p);
        }
        mesh.normals.reserve(vertexCount);
        const size_t normalsAt = kTerrainRegionHeaderSize + size_t(vertexCount) * 12;
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const size_t at = normalsAt + size_t(i) * 12;
            const glm::vec3 n(LoadF32(bytes, at), LoadF32(bytes, at + 4), LoadF32(bytes, at + 8));
            const double norm = glm::dot(glm::dvec3(n), glm::dvec3(n));
            if (!std::isfinite(norm) || std::abs(norm - 1.0) > 2e-6 || n.y <= 0.0f)
                return Fail<R>(Code::Corrupt, "{}: normal {} is not finite, not unit or points down", name, i);
            mesh.normals.push_back(n);
        }
        mesh.indices.reserve(indexCount);
        const size_t indicesAt = normalsAt + size_t(vertexCount) * 12;
        for (uint32_t i = 0; i < indexCount; ++i)
        {
            const uint32_t id = LoadU32(bytes, indicesAt + size_t(i) * 4);
            if (id >= vertexCount)
                return Fail<R>(Code::Corrupt, "{}: index {} is past the vertices", name, i);
            mesh.indices.push_back(id);
        }
        for (size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            const glm::vec3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]],
                            c = mesh.positions[mesh.indices[i + 2]];
            if (Cross(double(b.x) - a.x, double(b.z) - a.z, double(c.x) - a.x, double(c.z) - a.z) >= 0.0)
                return Fail<R>(Code::Corrupt, "{}: triangle {} is degenerate or wound the wrong way", name, i / 3);
        }
        if (lo != record.minHeight || hi != record.maxHeight)
            return Fail<R>(Code::Corrupt, "{}: heights span [{}, {}], the index says [{}, {}]", name, lo, hi,
                           record.minHeight, record.maxHeight);
        if (TerrainTopologyHash(mesh.positions, mesh.indices) != mesh.topologyHash)
            return Fail<R>(Code::Corrupt, "{}: topology hash differs", name);
        if (!TerrainRegionCoversDomain(grid, mesh))
            return Fail<R>(Code::Corrupt, "{}: triangles do not cover the Region's domain", name);
        return {std::move(mesh), {}};
    }

    std::optional<double> EvaluateTerrainRegion(const TerrainRegionMesh &mesh, double x, double z)
    {
        if (!std::isfinite(x) || !std::isfinite(z))
            return std::nullopt;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const std::array<glm::dvec3, 3> t{glm::dvec3(mesh.positions[mesh.indices[i]]),
                                              glm::dvec3(mesh.positions[mesh.indices[i + 1]]),
                                              glm::dvec3(mesh.positions[mesh.indices[i + 2]])};
            if (x < std::min({t[0].x, t[1].x, t[2].x}) || x > std::max({t[0].x, t[1].x, t[2].x}) ||
                z < std::min({t[0].z, t[1].z, t[2].z}) || z > std::max({t[0].z, t[1].z, t[2].z}))
                continue;
            bool inside = true;
            for (size_t j = 0; j < 3 && inside; ++j)
            {
                const glm::dvec3 a = t[j], b = t[(j + 1) % 3];
                const double side = Cross(b.x - a.x, b.z - a.z, x - a.x, z - a.z);
                const double scale = std::abs((b.x - a.x) * (z - a.z)) + std::abs((b.z - a.z) * (x - a.x));
                inside = side <= 64 * std::numeric_limits<double>::epsilon() * scale;
            }
            if (inside)
                return PlaneHeight(t, x, z);
        }
        return std::nullopt;
    }

    bool TerrainRegionCoversDomain(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh)
    {
        if (!grid.Contains(mesh.region) || mesh.indices.size() % 3 != 0)
            return false;
        const FloatDomain domain = DomainRelative(grid, mesh.region);
        for (const glm::vec3 &p : mesh.positions)
            if (!(p.x >= domain.minX && p.x <= domain.maxX && p.z >= domain.minZ && p.z <= domain.maxZ))
                return false;
        double area = 0.0;
        for (size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            if (mesh.indices[i] >= mesh.positions.size() || mesh.indices[i + 1] >= mesh.positions.size() ||
                mesh.indices[i + 2] >= mesh.positions.size())
                return false;
            const glm::dvec3 a(mesh.positions[mesh.indices[i]]), b(mesh.positions[mesh.indices[i + 1]]),
                c(mesh.positions[mesh.indices[i + 2]]);
            area += std::abs(Cross(b.x - a.x, b.z - a.z, c.x - a.x, c.z - a.z)) * 0.5;
        }
        const double expected = (double(domain.maxX) - domain.minX) * (double(domain.maxZ) - domain.minZ);
        return expected > 0.0 && std::abs(area - expected) <= 1e-9 * expected;
    }

    std::optional<double> TerrainRegionDeviation(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh,
                                                 const TerrainSourceLookup &sources)
    {
        if (!sources || !grid.Contains(mesh.region) || mesh.indices.size() % 3 != 0)
            return std::nullopt;
        const TerrainLayout &ref = grid.Reference();
        const TileRange tiles = grid.MapTiles(mesh.region);
        const glm::dvec3 origin = mesh.origin.Meters();
        const double tileSize = ref.TileSize();
        const int64_t width = int64_t(tiles.max.x) - tiles.min.x + 1;
        std::vector<std::vector<uint32_t>> buckets(size_t(width * (int64_t(tiles.max.z) - tiles.min.z + 1)));
        std::vector<glm::dvec3> local;
        local.reserve(mesh.positions.size());
        for (const glm::vec3 &p : mesh.positions)
            local.emplace_back(origin.x + double(p.x), double(p.y), origin.z + double(p.z));

        std::vector<std::pair<double, double>> extentX, extentZ;
        for (int32_t tx = tiles.min.x; tx <= tiles.max.x; ++tx)
        {
            const TerrainLocalBounds e = *ref.ValidExtent({tx, tiles.min.z});
            extentX.emplace_back(e.minX, e.maxX);
        }
        for (int32_t tz = tiles.min.z; tz <= tiles.max.z; ++tz)
        {
            const TerrainLocalBounds e = *ref.ValidExtent({tiles.min.x, tz});
            extentZ.emplace_back(e.minZ, e.maxZ);
        }
        // Tiles whose valid extent a closed range touches, from a one-tile margin around the estimate.
        auto touched = [&](double lo, double hi, int32_t first, const std::vector<std::pair<double, double>> &extent) {
            const int64_t count = int64_t(extent.size());
            const int64_t a = std::clamp<int64_t>(int64_t(std::floor(lo / tileSize)) - 1 - first, 0, count - 1);
            const int64_t b = std::clamp<int64_t>(int64_t(std::floor(hi / tileSize)) + 1 - first, 0, count - 1);
            std::pair<int64_t, int64_t> range{count, -1};
            for (int64_t t = a; t <= b; ++t)
                if (hi >= extent[size_t(t)].first && lo <= extent[size_t(t)].second)
                    range = {std::min(range.first, t), std::max(range.second, t)};
            return range;
        };

        double cellSpan = 0.0;
        for (uint32_t t = 0; t < mesh.indices.size(); t += 3)
        {
            double x0 = INFINITY, x1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
            for (uint32_t j = 0; j < 3; ++j)
            {
                if (mesh.indices[t + j] >= local.size())
                    return std::nullopt;
                const glm::dvec3 &v = local[mesh.indices[t + j]];
                x0 = std::min(x0, v.x);
                x1 = std::max(x1, v.x);
                z0 = std::min(z0, v.z);
                z1 = std::max(z1, v.z);
            }
            cellSpan = std::max({cellSpan, x1 - x0, z1 - z0});
            const auto [ax, bx] = touched(x0, x1, tiles.min.x, extentX);
            const auto [az, bz] = touched(z0, z1, tiles.min.z, extentZ);
            for (int64_t tz = az; tz <= bz; ++tz)
                for (int64_t tx = ax; tx <= bx; ++tx)
                    buckets[size_t(tz * width + tx)].push_back(t);
        }

        double bound = 0.0;
        for (int32_t tz = tiles.min.z; tz <= tiles.max.z; ++tz)
            for (int32_t tx = tiles.min.x; tx <= tiles.max.x; ++tx)
            {
                const TileCoord tile{tx, tz};
                const TerrainSourceTile source = sources(tile);
                if (!source.layout || !source.heights || source.heights->Tile() != tile ||
                    source.heights->IntervalsPerTile() != source.layout->IntervalsPerTile())
                    return std::nullopt;
                const TerrainLayout &layout = *source.layout;
                const TileHeights &heights = *source.heights;
                const int64_t firstX = layout.TileFirstSample(tx), firstZ = layout.TileFirstSample(tz);
                const double ox = layout.SamplePosition(firstX), oz = layout.SamplePosition(firstZ);
                const int64_t n = layout.IntervalsPerTile();
                const std::span<const float> samples = heights.Samples();
                double maxHeight = 1.0;
                for (float h : samples)
                {
                    if (!std::isfinite(h))
                        return std::nullopt;
                    maxHeight = std::max(maxHeight, std::abs(double(h)));
                }

                std::vector<glm::dvec3> general;
                double tileBound = 0.0;
                for (uint32_t t : buckets[size_t(int64_t(tz - tiles.min.z) * width + (tx - tiles.min.x))])
                {
                    std::array<glm::dvec3, 3> corner;
                    for (uint32_t j = 0; j < 3; ++j)
                    {
                        const glm::dvec3 &v = local[mesh.indices[t + j]];
                        corner[j] = {v.x - ox, v.y, v.z - oz};
                        maxHeight = std::max(maxHeight, std::abs(v.y));
                    }
                    // Half of a square cell on this tile's source grid holds whole source triangles.
                    std::array<int64_t, 3> ix{}, iz{};
                    bool aligned = true;
                    for (uint32_t j = 0; j < 3 && aligned; ++j)
                    {
                        const auto i = NodeOf(layout, firstX, ox, corner[j].x);
                        const auto k = NodeOf(layout, firstZ, oz, corner[j].z);
                        aligned = i && k;
                        if (aligned)
                        {
                            ix[j] = *i;
                            iz[j] = *k;
                        }
                    }
                    const int64_t i0 = std::min({ix[0], ix[1], ix[2]}), i1 = std::max({ix[0], ix[1], ix[2]});
                    const int64_t k0 = std::min({iz[0], iz[1], iz[2]}), k1 = std::max({iz[0], iz[1], iz[2]});
                    int upper = 0, lower = 0;
                    if (aligned && i1 > i0 && i1 - i0 == k1 - k0)
                        for (uint32_t j = 0; j < 3; ++j)
                        {
                            const bool a = ix[j] == i0 && iz[j] == k0, c = ix[j] == i1 && iz[j] == k1;
                            upper += a || c || (ix[j] == i0 && iz[j] == k1);
                            lower += a || c || (ix[j] == i1 && iz[j] == k0);
                        }
                    if (upper != 3 && lower != 3)
                    {
                        general.insert(general.end(), corner.begin(), corner.end());
                        continue;
                    }
                    for (int64_t k = std::max<int64_t>(k0, 0); k <= std::min<int64_t>(k1, n); ++k)
                        for (int64_t i = std::max<int64_t>(i0, 0); i <= std::min<int64_t>(i1, n); ++i)
                        {
                            const int64_t dx = i - i0, dz = k - k0;
                            if ((upper == 3 && dz < dx) || (lower == 3 && dz > dx))
                                continue;
                            const double x = layout.SamplePosition(firstX + i) - ox;
                            const double z = layout.SamplePosition(firstZ + k) - oz;
                            const double difference =
                                std::abs(PlaneHeight(corner, x, z) - double(samples[size_t(k * (n + 1) + i)]));
                            if (!std::isfinite(difference))
                                return std::nullopt;
                            tileBound = std::max(tileBound, difference);
                        }
                }
                const double margin = 4096 * std::numeric_limits<double>::epsilon() * maxHeight *
                                      (1 + (tileSize + 2 * cellSpan) / layout.SampleSpacing() +
                                       std::max(std::abs(ox), std::abs(oz)) / layout.SampleSpacing());
                if (!std::isfinite(margin))
                    return std::nullopt;
                bound = std::max(bound, std::nextafter(tileBound + margin, INFINITY));
                if (!general.empty())
                {
                    std::vector<uint32_t> ids(general.size());
                    for (uint32_t i = 0; i < ids.size(); ++i)
                        ids[i] = i;
                    const std::optional<double> overlay =
                        detail::OverlayDeviation(layout, heights, tile, general, ids, false);
                    if (!overlay)
                        return std::nullopt;
                    bound = std::max(bound, *overlay);
                }
            }
        if (!std::isfinite(bound))
            return std::nullopt;
        return bound;
    }
}
