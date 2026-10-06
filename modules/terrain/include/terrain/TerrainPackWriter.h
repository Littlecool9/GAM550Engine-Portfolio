/**
 * @file TerrainPackWriter.h
 * @author Jackey Cen
 * @brief Encodes a terrain release's index and packs in memory, and mints release ids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainByteSource.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TileHeights.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct TerrainPackWriteOptions
    {
    // Addresses never depend on it.
        int32_t chunkTiles = kDefaultTerrainChunkTiles;
        // Power of two up to kMaxTerrainPayloadAlignment.
        uint32_t payloadAlignment = kDefaultTerrainPayloadAlignment;
    };

    struct TerrainEncodedPack
    {
        TerrainChunkCoord chunk;
        std::string fileName;
        std::vector<std::byte> bytes;
    };

    struct TerrainEncodedRelease
    {
        std::vector<std::byte> index;
        std::vector<TerrainEncodedPack> packs;
        uint64_t heightsHash = 0;
    };

    struct TerrainTileWrite
    {
        const TileHeights *heights = nullptr;
        uint32_t layer = 0;
        bool source = true;
        std::optional<double> maxDeviation;
    };

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
                                                              const TerrainPackWriteOptions &options = {});

    uint64_t TerrainHeightsHash(std::span<const TerrainLayout> layers,
                                std::span<const TerrainTileWrite> tiles);

    // Single-layer convenience; exactly one source per map tile.
    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              const TerrainLayout &layout,
                                                              std::span<const TileHeights> tiles,
                                                              const TerrainPackWriteOptions &options = {});

    // Independent of chunk size and alignment.
    uint64_t TerrainHeightsHash(const TerrainLayout &layout, std::span<const TileHeights> tiles);

    // The nonce makes two bakes of the same source distinct releases. Never zero.
    TerrainReleaseId MintTerrainReleaseId(AssetId asset, TerrainSourceFingerprint source, uint64_t heightsHash,
                                          uint64_t nonce);

    uint64_t MakeTerrainReleaseNonce();

    std::shared_ptr<TerrainMemorySource> MakeTerrainMemorySource(const TerrainEncodedRelease &release);
}
