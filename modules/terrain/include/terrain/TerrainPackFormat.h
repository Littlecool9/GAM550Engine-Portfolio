/**
 * @file TerrainPackFormat.h
 * @author Jackey Cen
 * @brief Versioned terrain release index and tile pack format: records, parsing and payload decoding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // A release is one index file plus one pack file per storage chunk. Every
    // integer and float is little-endian, written byte by byte. All sizes are
    // multiples of 8 and every payload offset is a multiple of the index's
    // payloadAlignment. Byte offsets of format 1.0:
    //
    // Index header (120): 0 "MJTI", 4 u32 format, 8 u32 minor, 12 u32 headerSize,
    //   16 u32 contentVersion, 20 u32 payloadAlignment, 24 u64 asset, 32 u64 release,
    //   40 u64 sourceFingerprint, 48 f64 spacing, 56 i32 intervalsPerTile,
    //   60 i32 chunkTiles, 64 f64 minX, 72 minZ, 80 maxX, 88 maxZ, 96 u32 packCount,
    //   100 u32 packRecordSize, 104 u32 tileCount, 108 u32 tileRecordSize, 112 u64 heightsHash.
    // Then packCount pack records, tileCount tile records, optional extension
    // bytes, and a u64 Fnv1a64 trailer over every byte before it.
    // Pack record (32): 0 i32 chunkX, 4 i32 chunkZ, 8 u32 tileCount, 12 u32 zero,
    //   16 u64 fileSize, 24 u64 file checksum.
    // Tile record (88): 0 i32 tileX, 4 i32 tileZ, 8 u32 pack, 12 u32 samplesPerSide,
    //   16 u64 offset, 24 u64 length, 32 u64 payload checksum, 40 f64 spacing,
    //   48 f32 minHeight, 52 f32 maxHeight, 56 f64 valid minX, 64 minZ, 72 maxX, 80 maxZ.
    // Pack header (48): 0 "MJTP", 4 u32 format, 8 u32 minor, 12 u32 headerSize,
    //   16 u64 asset, 24 u64 release, 32 i32 chunkX, 36 i32 chunkZ, 40 u32 tileCount,
    //   44 u32 contentVersion; then the payloads.
    //
    // Compatible extension: a reader accepts any minor of its major format. A
    // newer minor may only grow headerSize and the record sizes by appending
    // fields, or add bytes between the tile table and the trailer; readers skip
    // what they do not know. Anything an older reader cannot safely skip bumps
    // the major format.
    inline constexpr TerrainFormatVersion kTerrainFormatVersion{1};
    inline constexpr uint32_t kTerrainFormatMinor = 0;

    // Tile payload layout: (n + 1)^2 float32, the TileHeights order. A new
    // payload encoding (quantized heights, LODs) bumps this, not the container.
    inline constexpr uint32_t kTerrainHeightsContentVersion = 1;

    inline constexpr uint32_t kTerrainIndexHeaderSize = 120;
    inline constexpr uint32_t kTerrainPackRecordSize = 32;
    inline constexpr uint32_t kTerrainTileRecordSize = 88;
    inline constexpr uint32_t kTerrainPackHeaderSize = 48;
    inline constexpr uint32_t kTerrainIndexTrailerSize = 8;

    inline constexpr uint32_t kDefaultTerrainPayloadAlignment = 16;
    inline constexpr uint32_t kMaxTerrainPayloadAlignment = 65536;
    // 16 x 16 tiles: 4096 m chunks of the 256 m prototype tiles; not frozen.
    inline constexpr int32_t kDefaultTerrainChunkTiles = 16;

    // Not .bin or .blob: the editor watcher and the cooker own those.
    inline constexpr std::string_view kTerrainIndexFileName = "release.tindex";

    // Storage chunk coordinates: floorDiv(tile, chunkTiles) on each axis.
    struct TerrainChunkCoord
    {
        int32_t x = 0;
        int32_t z = 0;

        constexpr bool operator==(const TerrainChunkCoord &) const = default;
    };

    // chunkTiles >= 1.
    TerrainChunkCoord TerrainChunkOf(TileCoord tile, int32_t chunkTiles);

    // "chunk_<x>_<z>.tpack" with signed decimal coordinates.
    std::string TerrainPackFileName(TerrainChunkCoord chunk);

    struct TerrainPackRecord
    {
        TerrainChunkCoord chunk;
        uint32_t tileCount = 0;
        uint64_t fileSize = 0;
        // Fnv1a64 of the whole pack file; the range reader never needs it.
        uint64_t checksum = 0;
    };

    struct TerrainTileRecord
    {
        TileCoord tile;
        uint32_t pack = 0;
        int32_t samplesPerSide = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        // Fnv1a64 of the payload bytes.
        uint64_t checksum = 0;
        double sampleSpacing = 0.0;
        // Over every stored sample, padding included.
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        TerrainLocalBounds validExtent;
    };

    enum class TerrainPackErrorCode : uint8_t
    {
        None,
        Io,            // a file is missing, unreadable or shorter than a record says
        Malformed,     // magic, sizes, bounds, ordering, duplicates, overlaps, overflow
        Unsupported,   // a major format, content version or limit this reader does not know
        Corrupt,       // a checksum or a payload disagrees with its record
        WrongIdentity, // asset or release differs from the one asked for
        NotFound,      // a tile outside the release's layout
        InvalidInput,  // the writer was given tiles or options it cannot encode
    };

    // Message names the file, record, tile and field, and expected against actual.
    struct TerrainPackError
    {
        TerrainPackErrorCode code = TerrainPackErrorCode::None;
        std::string message;
    };

    // A value or the reason there is none; never both.
    template <typename T>
    struct TerrainResult
    {
        std::optional<T> value;
        TerrainPackError error;

        bool Ok() const { return value.has_value(); }
        explicit operator bool() const { return Ok(); }
    };

    // A parsed and fully cross-checked index. Records are complete for the
    // layout and sorted by (z, x), so a tile's record is found by arithmetic.
    struct TerrainIndex
    {
        TerrainReleaseInfo release;
        uint32_t formatMinor = 0;
        uint32_t contentVersion = 0;
        uint32_t payloadAlignment = 0;
        int32_t chunkTiles = 0;
        TerrainLayout layout;
        // Fnv1a64 over every record's (tileX, tileZ, payload checksum), in record order.
        uint64_t heightsHash = 0;
        std::vector<TerrainPackRecord> packs;
        std::vector<TerrainTileRecord> tiles;

        // Nullptr outside the layout's tiles.
        const TerrainTileRecord *Find(TileCoord tile) const;
    };

    // Bounds-checks and cross-checks every field; never returns a partial index.
    TerrainResult<TerrainIndex> ParseTerrainIndex(std::span<const std::byte> bytes);

    // Checks one payload against its record (length, checksum, finite samples,
    // min/max) and returns its samples. Takes bytes from any reader.
    TerrainResult<TileHeights> DecodeTerrainTile(const TerrainIndex &index, const TerrainTileRecord &record,
                                                 std::span<const std::byte> payload);
}
