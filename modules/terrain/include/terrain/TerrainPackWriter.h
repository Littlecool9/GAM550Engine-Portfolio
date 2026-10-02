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
        // Tiles per storage-chunk side; one pack per chunk. Addresses never depend on it.
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

    // File bytes for one release directory, in the order a reader needs them.
    struct TerrainEncodedRelease
    {
        std::vector<std::byte> index;
        std::vector<TerrainEncodedPack> packs;
        uint64_t heightsHash = 0;
    };

    // Pure: no disk. Needs exactly one tile per layout tile (any order), each
    // with the layout's n and finite heights; asset and release must be set and
    // the format must be kTerrainFormatVersion. The bake writes the result with
    // core FileIO; tests and fixtures use the same bytes.
    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              const TerrainLayout &layout,
                                                              std::span<const TileHeights> tiles,
                                                              const TerrainPackWriteOptions &options = {});

    // The heightsHash EncodeTerrainRelease records, without packing. It depends
    // on heights and tile coordinates only, not on chunk size or alignment.
    uint64_t TerrainHeightsHash(std::span<const TileHeights> tiles);

    // Minted once per bake, before encoding, and recorded in the index and every
    // pack header. The nonce makes two bakes of the same source distinct
    // releases; pass a fixed one for reproducible tests. Never zero.
    TerrainReleaseId MintTerrainReleaseId(AssetId asset, TerrainSourceFingerprint source, uint64_t heightsHash,
                                          uint64_t nonce);

    // Wall clock mixed with std::random_device, for MintTerrainReleaseId.
    uint64_t MakeTerrainReleaseNonce();

    // Every encoded file under its release-relative name.
    std::shared_ptr<TerrainMemorySource> MakeTerrainMemorySource(const TerrainEncodedRelease &release);
}
