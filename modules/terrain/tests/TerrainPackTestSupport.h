/**
 * @file TerrainPackTestSupport.h
 * @author Jackey Cen
 * @brief Shared release builders and byte patching for the terrain pack tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>

#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
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
}
