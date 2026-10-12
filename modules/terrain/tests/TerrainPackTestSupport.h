/**
 * @file TerrainPackTestSupport.h
 * @author Jackey Cen
 * @brief Shared release builders and byte patching for the terrain pack tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLod.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainRelease.h>

#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir::test
{
    inline AssetId TerrainAsset(std::string_view relative)
    {
        const AssetPathResult logical = MakeLogicalAssetPath("games/TerrainFixture/assets", relative);
        REQUIRE(logical.Ok());
        return MakeAssetId(logical.path);
    }

    // Exact in float here; a function of global indices, so neighbours share edges.
    inline float AnalyticHeight(int64_t i, int64_t k, float salt = 0.0f)
    {
        return static_cast<float>(i * 3 - k * 5) * 0.25f + static_cast<float>((i * k) % 7) * 0.125f + salt;
    }

    inline std::vector<TileHeights> BuildTiles(const TerrainLayout &layout, float salt = 0.0f)
    {
        std::vector<TileHeights> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
        {
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, n);
                REQUIRE(tile);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        tile->Set(i, k, AnalyticHeight(layout.TileFirstSample(x) + i, layout.TileFirstSample(z) + k,
                                                       salt));
                tiles.push_back(std::move(*tile));
            }
        }
        return tiles;
    }

    inline TerrainEncodedRelease Encode(const TerrainReleaseInfo &info, const TerrainLayout &layout,
                                        std::span<const TileHeights> tiles, TerrainPackWriteOptions options = {})
    {
        TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layout, tiles, options);
        INFO(encoded.error.message);
        REQUIRE(encoded);
        return std::move(*encoded.value);
    }

    inline TerrainReleaseInfo Info(AssetId asset, uint64_t release, uint64_t source = 0x5eed)
    {
        return {asset, TerrainSourceFingerprint(source), kTerrainFormatVersion, TerrainReleaseId(release)};
    }

    // Tiles (-2..0, -1..1), every outer tile clipped.
    inline TerrainLayout EdgeLayout()
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-300.5, -10.0, 100.25, 260.0});
        REQUIRE(layout);
        return *layout;
    }

    // relief-2x2.terrain.json's layout.
    inline TerrainLayout ReliefLayout()
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-250.0, -256.0, 256.0, 250.0});
        REQUIRE(layout);
        return *layout;
    }

    // Two tiles per chunk: packs chunk_-1_-1, chunk_0_-1, chunk_-1_0 (four tiles) and chunk_0_0.
    inline TerrainEncodedRelease EncodeEdge(AssetId asset, uint64_t release, float salt = 0.0f)
    {
        const TerrainLayout layout = EdgeLayout();
        const std::vector<TileHeights> tiles = BuildTiles(layout, salt);
        return Encode(Info(asset, release), layout, tiles, {.chunkTiles = 2});
    }

    inline TerrainReleaseHandle OpenOk(TerrainResult<TerrainReleaseHandle> opened)
    {
        INFO(opened.error.message);
        REQUIRE(opened);
        return std::move(*opened.value);
    }

    struct OpenedRelease
    {
        std::shared_ptr<TerrainMemorySource> source;
        TerrainReleaseHandle handle;
    };

    inline OpenedRelease OpenInMemory(const TerrainEncodedRelease &encoded)
    {
        std::shared_ptr<TerrainMemorySource> source = MakeTerrainMemorySource(encoded);
        return {source, OpenOk(TerrainReleaseHandle::Open(source))};
    }

    inline TileHeights ReadOk(const TerrainReleaseHandle &handle, TileCoord tile)
    {
        TerrainResult<TileHeights> read = handle.ReadTile({handle.Info().asset, tile});
        INFO(read.error.message);
        REQUIRE(read);
        return std::move(*read.value);
    }

    inline bool Mentions(const TerrainPackError &error, std::string_view text)
    {
        UNSCOPED_INFO(error.message);
        return error.message.find(text) != std::string::npos;
    }

    inline void StoreU32(std::vector<std::byte> &b, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }

    inline void StoreU64(std::vector<std::byte> &b, size_t at, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }

    inline uint64_t LoadU64(std::span<const std::byte> b, size_t at)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(std::to_integer<uint8_t>(b[at + i])) << (8 * i);
        return v;
    }

    // Recomputes the trailer so an edit reaches the check it targets.
    inline void ResealIndex(std::vector<std::byte> &index)
    {
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));
    }

    inline size_t TileRecordAt(uint32_t packCount, uint32_t tile)
    {
        return kTerrainIndexHeaderSize + size_t(packCount) * kTerrainPackRecordSize +
               size_t(tile) * kTerrainTileRecordSize;
    }

    inline std::filesystem::path MakeScratchDir(std::string_view tag)
    {
        static std::atomic<int> counter{0};
        std::random_device rd;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("mjolnir_terrain_" + std::string(tag) + "_" + std::to_string(rd()) + "_" +
             std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir);
        return dir;
    }

    inline std::filesystem::path WriteRelease(const std::filesystem::path &root, const TerrainReleaseInfo &info,
                                              const TerrainEncodedRelease &encoded)
    {
        const std::filesystem::path dir = TerrainReleaseDir(root, info.asset, info.release);
        std::filesystem::create_directories(dir);
        REQUIRE(WriteBytes(dir / kTerrainIndexFileName, encoded.index));
        for (const TerrainEncodedPack &pack : encoded.packs)
            REQUIRE(WriteBytes(dir / pack.fileName, pack.bytes));
        return dir;
    }

    inline void PointCurrentAt(const std::filesystem::path &root, AssetId asset, TerrainReleaseId release)
    {
        REQUIRE(WriteTextAtomic(TerrainCurrentPointerPath(root, asset), EncodeTerrainCurrentPointer(release)));
    }

    // Plain grid meshes on every Region's lines, each node at the sample of the tile holding it.
    inline TerrainRegionWrites BuildTestRegions(const TerrainLayout &layout, std::span<const TileHeights> tiles,
                                                uint32_t regionTiles, uint32_t cells)
    {
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, regionTiles, cells, 1);
        REQUIRE(grid);
        std::unordered_map<TileCoord, const TileHeights *> byTile;
        std::vector<TerrainSourceChecksum> checksums;
        for (const TileHeights &tile : tiles)
        {
            byTile[tile.Tile()] = &tile;
            checksums.push_back({tile.Tile(), TerrainFinalHeightChecksum(tile)});
        }
        const TileRange map = layout.Tiles();
        const int64_t n = layout.IntervalsPerTile();
        auto floorDiv = [](int64_t v, int64_t d) { return v / d - ((v % d != 0) && (v < 0) ? 1 : 0); };
        auto height = [&](int64_t i, int64_t k) {
            const TileCoord tile{int32_t(std::clamp<int64_t>(floorDiv(i, n), map.min.x, map.max.x)),
                                 int32_t(std::clamp<int64_t>(floorDiv(k, n), map.min.z, map.max.z))};
            return byTile.at(tile)->At(int32_t(i - tile.x * n), int32_t(k - tile.z * n));
        };
        const TerrainSourceLookup sources = [&](TileCoord tile) {
            const auto it = byTile.find(tile);
            return it == byTile.end() ? TerrainSourceTile{} : TerrainSourceTile{&layout, it->second};
        };

        TerrainRegionWrites writes;
        writes.grid = grid;
        for (uint64_t ordinal = 0; ordinal < grid->RegionCount(); ++ordinal)
        {
            const TerrainRegionCoord region = grid->AtOrdinal(ordinal);
            const TerrainLocalBounds rect = grid->Rect(region);
            TerrainRegionMesh mesh;
            mesh.region = region;
            mesh.tiles = grid->MapTiles(region);
            mesh.cellSpacing = grid->CellSpacing(region.level);
            mesh.origin = *TerrainLocalPosition::FromMeters({rect.minX, 0.0, rect.minZ});
            const std::vector<int64_t> lx = grid->LinesX(region), lz = grid->LinesZ(region);
            for (int64_t k : lz)
                for (int64_t i : lx)
                {
                    mesh.positions.emplace_back(float(layout.SamplePosition(i) - rect.minX), height(i, k),
                                                float(layout.SamplePosition(k) - rect.minZ));
                    mesh.normals.emplace_back(0.0f, 1.0f, 0.0f);
                }
            const uint32_t row = uint32_t(lx.size());
            for (uint32_t k = 0; k + 1 < lz.size(); ++k)
                for (uint32_t i = 0; i + 1 < row; ++i)
                {
                    const uint32_t a = k * row + i, b = a + 1, d = a + row, c = d + 1;
                    for (uint32_t id : {a, d, c, a, c, b})
                        mesh.indices.push_back(id);
                }
            mesh.topologyHash = TerrainTopologyHash(mesh.positions, mesh.indices);
            mesh.sourceChecksum = TerrainRegionSourceChecksums(*grid, region.level, checksums)
                [size_t(region.z) * size_t(grid->Count(region.level).x) + size_t(region.x)];
            const std::optional<double> deviation = TerrainRegionDeviation(*grid, mesh, sources);
            REQUIRE(deviation);
            mesh.maxDeviation = *deviation;

            TerrainTileRecord record;
            record.tile = {region.x, region.z};
            record.level = region.level;
            record.kind = TerrainPayloadKind::Region;
            record.source = false;
            record.samplesPerSide = grid->SamplesPerSide(region.level);
            record.sampleSpacing = mesh.cellSpacing;
            const auto [lo, hi] = std::minmax_element(mesh.positions.begin(), mesh.positions.end(),
                                                      [](const glm::vec3 &p, const glm::vec3 &q) { return p.y < q.y; });
            record.minHeight = lo->y;
            record.maxHeight = hi->y;
            record.validExtent = grid->ValidExtent(region);
            record.maxDeviation = mesh.maxDeviation;
            record.sourceChecksum = mesh.sourceChecksum;
            record.topologyHash = mesh.topologyHash;
            record.vertexCount = uint32_t(mesh.positions.size());
            record.indexCount = uint32_t(mesh.indices.size());
            writes.payloads.push_back({record, EncodeTerrainRegion(mesh)});
        }
        return writes;
    }

    // A LodHeight and a flat-normal Normal payload per tile and layer; layer 0 is the source.
    inline std::vector<TerrainPayloadWrite> BuildTestLods(std::span<const TerrainLayout> layers,
                                                          std::span<const TileHeights> tiles)
    {
        std::vector<TerrainPayloadWrite> writes;
        const TerrainLayout &source = layers[0];
        for (const TileHeights &tile : tiles)
            for (uint32_t level = 0; level < layers.size(); ++level)
            {
                const TerrainLayout &layout = layers[level];
                const int32_t n = layout.IntervalsPerTile(), stride = source.IntervalsPerTile() / n;
                TerrainLodSurface lod;
                lod.tile = tile.Tile();
                lod.layer = level;
                lod.level = level;
                lod.sourceSpacing = source.SampleSpacing();
                lod.spacing = layout.SampleSpacing();
                lod.sourceChecksum = TerrainFinalHeightChecksum(tile);
                lod.origin = *TerrainLocalPosition::FromMeters(
                    {source.SamplePosition(source.TileFirstSample(tile.Tile().x)), 0.0,
                     source.SamplePosition(source.TileFirstSample(tile.Tile().z))});
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        lod.positions.emplace_back(float(i * layout.SampleSpacing()), tile.At(i * stride, k * stride),
                                                   float(k * layout.SampleSpacing()));
                for (int32_t k = 0; k < n; ++k)
                    for (int32_t i = 0; i < n; ++i)
                    {
                        const uint32_t a = uint32_t(k * (n + 1) + i), b = a + 1, d = a + uint32_t(n + 1), c = d + 1;
                        for (uint32_t id : {a, d, c, a, c, b})
                            lod.indices.push_back(id);
                    }
                lod.topologyHash = TerrainTopologyHash(lod.positions, lod.indices);
                const std::optional<double> deviation = TerrainLodDeviation(source, tile, lod);
                REQUIRE(deviation);
                lod.maxDeviation = *deviation;
                const auto [lo, hi] = std::minmax_element(lod.positions.begin(), lod.positions.end(),
                                                          [](const glm::vec3 &p, const glm::vec3 &q) { return p.y < q.y; });

                TerrainTileRecord record;
                record.tile = tile.Tile();
                record.layer = level;
                record.level = level;
                record.kind = TerrainPayloadKind::LodHeight;
                record.source = false;
                record.samplesPerSide = n + 1;
                record.sampleSpacing = layout.SampleSpacing();
                record.minHeight = lo->y;
                record.maxHeight = hi->y;
                record.validExtent = *layout.ValidExtent(tile.Tile());
                record.maxDeviation = lod.maxDeviation;
                record.sourceChecksum = lod.sourceChecksum;
                record.topologyHash = lod.topologyHash;
                record.vertexCount = uint32_t(lod.positions.size());
                record.indexCount = uint32_t(lod.indices.size());
                writes.push_back({record, EncodeTerrainLod(lod)});
                record.kind = TerrainPayloadKind::Normal;
                writes.push_back(
                    {record, EncodeTerrainNormals(lod, std::vector<glm::vec3>(lod.positions.size(), {0.0f, 1.0f, 0.0f}))});
            }
        return writes;
    }

    inline TerrainEncodedRelease EncodeWithRegions(const TerrainReleaseInfo &info, const TerrainLayout &layout,
                                                   std::span<const TileHeights> tiles,
                                                   const TerrainRegionWrites &regions,
                                                   TerrainPackWriteOptions options = {})
    {
        std::vector<TerrainTileWrite> inputs;
        for (const TileHeights &tile : tiles)
            inputs.push_back({&tile});
        TerrainResult<TerrainEncodedRelease> encoded =
            EncodeTerrainRelease(info, std::span(&layout, 1), inputs, {}, regions, options);
        INFO(encoded.error.message);
        REQUIRE(encoded);
        return std::move(*encoded.value);
    }

    inline size_t RegionRecordAt(const TerrainIndex &index, uint32_t region)
    {
        return kTerrainIndexHeaderSize + index.packs.size() * kTerrainPackRecordSize +
               index.tiles.size() * kTerrainTileRecordSize + index.layers.size() * kTerrainLayerRecordSize +
               size_t(region) * kTerrainTileRecordSize;
    }
}
