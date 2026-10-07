/**
 * @file TerrainPackFormat.cpp
 * @author Jackey Cen
 * @brief Terrain release index and tile pack format: records, parsing and decoding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainPackFormat.h"

#include "LittleEndian.h"
#include "PackInternal.h"

#include <assets/AssetId.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Error;
        using detail::Fail;
        using detail::Hex;
        using detail::LoadF32;
        using detail::LoadF64;
        using detail::LoadI32;
        using detail::LoadU32;
        using detail::LoadU64;

        int32_t FloorDiv(int32_t value, int32_t divisor)
        {
            int32_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        bool IsPowerOfTwo(uint32_t v)
        {
            return v != 0 && (v & (v - 1)) == 0;
        }

        std::string DescribeBounds(const TerrainLocalBounds &b)
        {
            return std::format("[{}, {}]..[{}, {}]", b.minX, b.minZ, b.maxX, b.maxZ);
        }

        struct Header
        {
            uint32_t headerSize = 0;
            uint32_t minor = 0;
            uint32_t contentVersion = 0;
            uint32_t alignment = 0;
            uint64_t asset = 0;
            uint64_t release = 0;
            uint64_t source = 0;
            double spacing = 0.0;
            int32_t intervals = 0;
            int32_t chunkTiles = 0;
            TerrainLocalBounds bounds;
            uint32_t packCount = 0;
            uint32_t packRecordSize = 0;
            uint32_t tileCount = 0;
            uint32_t tileRecordSize = 0;
            uint64_t heightsHash = 0;
        };

        TerrainResult<Header> ParseHeader(std::span<const std::byte> bytes)
        {
            constexpr size_t kPrefix = 16;
            if (bytes.size() < kPrefix)
                return Fail<Header>(Code::Malformed, "{} bytes is too short for an index header (truncated?)",
                                    bytes.size());
            if (!std::equal(detail::kIndexMagic.begin(), detail::kIndexMagic.end(), bytes.begin()))
                return Fail<Header>(Code::Malformed, "bad magic: not a terrain release index");

            const uint32_t format = LoadU32(bytes, 4);
            if (format != kTerrainFormatVersion.value)
                return Fail<Header>(Code::Unsupported,
                                    "format version {} is not supported (this reader reads {}); rebake it with a "
                                    "matching Terrain Tool",
                                    format, kTerrainFormatVersion.value);

            Header h;
            h.minor = LoadU32(bytes, 8);
            h.headerSize = LoadU32(bytes, 12);
            if (h.headerSize < kTerrainIndexHeaderSize || h.headerSize % 8 != 0)
                return Fail<Header>(Code::Malformed, "header size {} is invalid (needs >= {} and a multiple of 8)",
                                    h.headerSize, kTerrainIndexHeaderSize);
            if (bytes.size() < uint64_t(h.headerSize) + kTerrainIndexTrailerSize)
                return Fail<Header>(Code::Malformed,
                                    "{} bytes cannot hold a {}-byte header and the checksum (truncated?)",
                                    bytes.size(), h.headerSize);

            // Checked before any table is read, so an edited index never yields records.
            const size_t body = bytes.size() - kTerrainIndexTrailerSize;
            const uint64_t stored = LoadU64(bytes, body);
            const uint64_t actual = Fnv1a64(bytes.first(body));
            if (stored != actual)
                return Fail<Header>(Code::Corrupt,
                                    "checksum {} does not match the contents {} (truncated or corrupt index)",
                                    Hex(stored), Hex(actual));

            h.contentVersion = LoadU32(bytes, 16);
            if (h.contentVersion != kTerrainHeightsContentVersion)
                return Fail<Header>(Code::Unsupported, "content version {} is not supported (this reader reads {})",
                                    h.contentVersion, kTerrainHeightsContentVersion);
            h.alignment = LoadU32(bytes, 20);
            h.asset = LoadU64(bytes, 24);
            h.release = LoadU64(bytes, 32);
            h.source = LoadU64(bytes, 40);
            h.spacing = LoadF64(bytes, 48);
            h.intervals = LoadI32(bytes, 56);
            h.chunkTiles = LoadI32(bytes, 60);
            h.bounds = {LoadF64(bytes, 64), LoadF64(bytes, 72), LoadF64(bytes, 80), LoadF64(bytes, 88)};
            h.packCount = LoadU32(bytes, 96);
            h.packRecordSize = LoadU32(bytes, 100);
            h.tileCount = LoadU32(bytes, 104);
            h.tileRecordSize = LoadU32(bytes, 108);
            h.heightsHash = LoadU64(bytes, 112);

            if (!IsPowerOfTwo(h.alignment) || h.alignment > kMaxTerrainPayloadAlignment)
                return Fail<Header>(Code::Malformed, "payload alignment {} is not a power of two up to {}",
                                    h.alignment, kMaxTerrainPayloadAlignment);
            if (h.asset == 0)
                return Fail<Header>(Code::Malformed, "asset id is zero");
            if (h.release == 0)
                return Fail<Header>(Code::Malformed, "release id is zero");
            if (h.packRecordSize < kTerrainPackRecordSize || h.packRecordSize % 8 != 0)
                return Fail<Header>(Code::Malformed, "pack record size {} is invalid (needs >= {}, a multiple of 8)",
                                    h.packRecordSize, kTerrainPackRecordSize);
            if (h.tileRecordSize < kTerrainTileRecordSize || h.tileRecordSize % 8 != 0)
                return Fail<Header>(Code::Malformed, "tile record size {} is invalid (needs >= {}, a multiple of 8)",
                                    h.tileRecordSize, kTerrainTileRecordSize);

            // Each product is below 2^64; subtracting keeps the sum from overflowing.
            const uint64_t room = body - h.headerSize;
            const uint64_t packBytes = uint64_t(h.packCount) * h.packRecordSize;
            const uint64_t tileBytes = uint64_t(h.tileCount) * h.tileRecordSize;
            if (packBytes > room || tileBytes > room - packBytes)
                return Fail<Header>(Code::Malformed, "{} pack and {} tile records do not fit in {} bytes (truncated?)",
                                    h.packCount, h.tileCount, bytes.size());

            if (h.intervals < 1)
                return Fail<Header>(Code::Malformed, "intervals per tile {} is below 1", h.intervals);
            if (h.intervals > kMaxTerrainTileIntervals)
                return Fail<Header>(Code::Unsupported, "intervals per tile {} exceeds this reader's limit {}",
                                    h.intervals, kMaxTerrainTileIntervals);
            if (h.chunkTiles < 1)
                return Fail<Header>(Code::Malformed, "tiles per chunk {} is below 1", h.chunkTiles);

            TerrainResult<Header> result;
            result.value = h;
            return result;
        }

        TerrainPackRecord ReadPackRecord(std::span<const std::byte> b, size_t at)
        {
            TerrainPackRecord r;
            r.chunk = {LoadI32(b, at), LoadI32(b, at + 4)};
            r.tileCount = LoadU32(b, at + 8);
            r.fileSize = LoadU64(b, at + 16);
            r.checksum = LoadU64(b, at + 24);
            return r;
        }

        TerrainTileRecord ReadTileRecord(std::span<const std::byte> b, size_t at)
        {
            TerrainTileRecord r;
            r.tile = {LoadI32(b, at), LoadI32(b, at + 4)};
            r.pack = LoadU32(b, at + 8);
            r.samplesPerSide = static_cast<int32_t>(LoadU32(b, at + 12));
            r.offset = LoadU64(b, at + 16);
            r.length = LoadU64(b, at + 24);
            r.checksum = LoadU64(b, at + 32);
            r.sampleSpacing = LoadF64(b, at + 40);
            r.minHeight = LoadF32(b, at + 48);
            r.maxHeight = LoadF32(b, at + 52);
            r.validExtent = {LoadF64(b, at + 56), LoadF64(b, at + 64), LoadF64(b, at + 72), LoadF64(b, at + 80)};
            return r;
        }

        TerrainPackError CheckTileRecord(const Header &h, const TerrainLayout &layout,
                                         std::span<const TerrainPackRecord> packs, const TerrainTileRecord &r)
        {
            const std::string tile = Describe(r.tile);
            if (r.pack >= packs.size())
                return Error(Code::Malformed, "tile {}: pack {} is out of range ({} packs)", tile, r.pack,
                             packs.size());
            const TerrainPackRecord &pack = packs[r.pack];
            const std::string name = TerrainPackFileName(pack.chunk);
            const TerrainChunkCoord chunk = TerrainChunkOf(r.tile, h.chunkTiles);
            if (pack.chunk != chunk)
                return Error(Code::Malformed, "tile {}: stored in {} but belongs to chunk {} at {} tiles per chunk",
                             tile, name, Describe(chunk), h.chunkTiles);

            const int32_t side = h.intervals + 1;
            if (r.samplesPerSide != side || r.sampleSpacing != h.spacing)
                return Error(Code::Malformed, "tile {}: {} samples per side at spacing {}, the layout has {} at {}",
                             tile, r.samplesPerSide, r.sampleSpacing, side, h.spacing);
            const uint64_t length = uint64_t(side) * uint64_t(side) * sizeof(float);
            if (r.length != length)
                return Error(Code::Malformed, "tile {}: payload length {} should be {}", tile, r.length, length);
            if (r.offset % h.alignment != 0)
                return Error(Code::Malformed, "tile {}: offset {} is not a multiple of the payload alignment {}", tile,
                             r.offset, h.alignment);
            if (r.offset < kTerrainPackHeaderSize)
                return Error(Code::Malformed, "tile {}: offset {} is inside the pack header of {}", tile, r.offset,
                             name);
            if (r.offset > std::numeric_limits<uint64_t>::max() - r.length)
                return Error(Code::Malformed, "tile {}: offset {} + length {} overflows", tile, r.offset, r.length);
            if (r.offset + r.length > pack.fileSize)
                return Error(Code::Malformed, "tile {}: payload [{}, {}) ends past {} ({} bytes)", tile, r.offset,
                             r.offset + r.length, name, pack.fileSize);
            if (!std::isfinite(r.minHeight) || !std::isfinite(r.maxHeight) || r.minHeight > r.maxHeight)
                return Error(Code::Malformed, "tile {}: height range [{}, {}] is invalid", tile, r.minHeight,
                             r.maxHeight);
            const TerrainLocalBounds extent = *layout.ValidExtent(r.tile);
            if (r.validExtent != extent)
                return Error(Code::Malformed, "tile {}: valid extent {} differs from the layout's {}", tile,
                             DescribeBounds(r.validExtent), DescribeBounds(extent));
            return {};
        }
    }

    namespace detail
    {
        uint64_t HeightsHashOf(std::span<const TerrainTileRecord> records)
        {
            std::vector<std::byte> bytes;
            bytes.reserve(records.size() * 16);
            LeWriter out(bytes);
            for (const TerrainTileRecord &r : records)
            {
                out.I32(r.tile.x);
                out.I32(r.tile.z);
                out.U64(r.checksum);
            }
            return Fnv1a64(bytes);
        }
    }

    TerrainChunkCoord TerrainChunkOf(TileCoord tile, int32_t chunkTiles)
    {
        return {FloorDiv(tile.x, chunkTiles), FloorDiv(tile.z, chunkTiles)};
    }

    std::string TerrainPackFileName(TerrainChunkCoord chunk)
    {
        return std::format("chunk_{}_{}.tpack", chunk.x, chunk.z);
    }

    const TerrainTileRecord *TerrainIndex::Find(TileCoord tile) const
    {
        if (!layout.Contains(tile))
            return nullptr;
        const TileRange range = layout.Tiles();
        const int64_t width = int64_t(range.max.x) - range.min.x + 1;
        const int64_t at = (int64_t(tile.z) - range.min.z) * width + (int64_t(tile.x) - range.min.x);
        return &tiles[static_cast<size_t>(at)];
    }

    TerrainResult<TerrainIndex> ParseTerrainIndex(std::span<const std::byte> bytes)
    {
        using R = TerrainIndex;
        TerrainResult<Header> parsed = ParseHeader(bytes);
        if (!parsed)
            return detail::Failure<R>(parsed.error, "");
        const Header &h = *parsed.value;

        const std::optional<TerrainLayout> layout = TerrainLayout::Create(h.spacing, h.intervals, h.bounds);
        if (!layout)
            return Fail<R>(Code::Malformed, "layout (spacing {}, {} intervals per tile, bounds {}) is invalid",
                           h.spacing, h.intervals, DescribeBounds(h.bounds));

        std::vector<TerrainPackRecord> packs(h.packCount);
        for (uint32_t p = 0; p < h.packCount; ++p)
        {
            TerrainPackRecord &pack = packs[p];
            pack = ReadPackRecord(bytes, size_t(h.headerSize) + size_t(p) * h.packRecordSize);
            if (p > 0 && !detail::LessZX(packs[p - 1].chunk.x, packs[p - 1].chunk.z, pack.chunk.x, pack.chunk.z))
                return Fail<R>(Code::Malformed,
                               "pack records {} and {} (chunks {} and {}) are not in increasing (z, x) order or "
                               "name one chunk twice",
                               p - 1, p, Describe(packs[p - 1].chunk), Describe(pack.chunk));
            if (pack.tileCount == 0)
                return Fail<R>(Code::Malformed, "pack record {} ({}) holds no tiles", p,
                               TerrainPackFileName(pack.chunk));
            if (pack.fileSize < kTerrainPackHeaderSize)
                return Fail<R>(Code::Malformed, "pack record {} ({}): file size {} is smaller than a pack header", p,
                               TerrainPackFileName(pack.chunk), pack.fileSize);
        }

        const TileRange range = layout->Tiles();
        const uint64_t width = uint64_t(int64_t(range.max.x) - range.min.x + 1);
        const uint64_t height = uint64_t(int64_t(range.max.z) - range.min.z + 1);
        if (width > std::numeric_limits<uint32_t>::max() / height || width * height != h.tileCount)
            return Fail<R>(Code::Malformed,
                           "{} tile records, but the layout has {} x {} tiles from {} to {}; a release lists every "
                           "tile once",
                           h.tileCount, width, height, Describe(range.min), Describe(range.max));

        const size_t tileTable = size_t(h.headerSize) + size_t(h.packCount) * h.packRecordSize;
        std::vector<TerrainTileRecord> tiles(h.tileCount);
        std::vector<uint32_t> tilesPerPack(h.packCount, 0);
        for (uint32_t t = 0; t < h.tileCount; ++t)
        {
            TerrainTileRecord &r = tiles[t];
            r = ReadTileRecord(bytes, tileTable + size_t(t) * h.tileRecordSize);
            const TileCoord expected{static_cast<int32_t>(range.min.x + int64_t(t % width)),
                                     static_cast<int32_t>(range.min.z + int64_t(t / width))};
            if (r.tile != expected)
            {
                if (t > 0 && r.tile == tiles[t - 1].tile)
                    return Fail<R>(Code::Malformed, "tile records {} and {} both name tile {}", t - 1, t,
                                   Describe(r.tile));
                return Fail<R>(Code::Malformed,
                               "tile record {} names tile {}, expected {}: records cover the layout once in (z, x) "
                               "order",
                               t, Describe(r.tile), Describe(expected));
            }
            const TerrainPackError checked = CheckTileRecord(h, *layout, packs, r);
            if (checked.code != Code::None)
                return detail::Failure<R>(checked, "");
            ++tilesPerPack[r.pack];
        }

        for (uint32_t p = 0; p < h.packCount; ++p)
        {
            if (tilesPerPack[p] != packs[p].tileCount)
                return Fail<R>(Code::Malformed, "pack record {} ({}) says {} tiles, {} tile records point at it", p,
                               TerrainPackFileName(packs[p].chunk), packs[p].tileCount, tilesPerPack[p]);
        }

        // Payloads never share bytes: no two ranges in one pack may overlap.
        std::vector<uint32_t> byOffset(h.tileCount);
        for (uint32_t t = 0; t < h.tileCount; ++t)
            byOffset[t] = t;
        std::sort(byOffset.begin(), byOffset.end(), [&](uint32_t a, uint32_t b) {
            return tiles[a].pack != tiles[b].pack ? tiles[a].pack < tiles[b].pack : tiles[a].offset < tiles[b].offset;
        });
        for (size_t i = 1; i < byOffset.size(); ++i)
        {
            const TerrainTileRecord &a = tiles[byOffset[i - 1]];
            const TerrainTileRecord &b = tiles[byOffset[i]];
            if (a.pack == b.pack && a.offset + a.length > b.offset)
                return Fail<R>(Code::Malformed, "tiles {} and {} overlap in {}: [{}, {}) and [{}, {})",
                               Describe(a.tile), Describe(b.tile), TerrainPackFileName(packs[a.pack].chunk), a.offset,
                               a.offset + a.length, b.offset, b.offset + b.length);
        }

        const uint64_t heightsHash = detail::HeightsHashOf(tiles);
        if (heightsHash != h.heightsHash)
            return Fail<R>(Code::Corrupt, "heights hash {} does not match the tile records {}", Hex(h.heightsHash),
                           Hex(heightsHash));

        TerrainResult<TerrainIndex> result;
        result.value.emplace(TerrainIndex{
            .release = {AssetId{h.asset}, TerrainSourceFingerprint(h.source), kTerrainFormatVersion,
                        TerrainReleaseId(h.release)},
            .formatMinor = h.minor,
            .contentVersion = h.contentVersion,
            .payloadAlignment = h.alignment,
            .chunkTiles = h.chunkTiles,
            .layout = *layout,
            .heightsHash = heightsHash,
            .packs = std::move(packs),
            .tiles = std::move(tiles),
        });
        return result;
    }

    TerrainResult<TileHeights> DecodeTerrainTile(const TerrainIndex &index, const TerrainTileRecord &record,
                                                 std::span<const std::byte> payload)
    {
        using R = TileHeights;
        const std::string tile = Describe(record.tile);
        const int32_t intervals = index.layout.IntervalsPerTile();
        if (record.samplesPerSide != intervals + 1)
            return Fail<R>(Code::Malformed, "tile {}: {} samples per side, the layout has {}", tile,
                           record.samplesPerSide, intervals + 1);
        if (payload.size() != record.length)
            return Fail<R>(Code::Io, "tile {}: got {} payload bytes, the index says {}", tile, payload.size(),
                           record.length);
        const uint64_t checksum = Fnv1a64(payload);
        if (checksum != record.checksum)
            return Fail<R>(Code::Corrupt,
                           "tile {}: payload checksum {} does not match the index {}; the pack is corrupt or from "
                           "another release",
                           tile, Hex(checksum), Hex(record.checksum));

        const size_t side = static_cast<size_t>(record.samplesPerSide);
        std::vector<float> samples(side * side);
        float lo = std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (size_t s = 0; s < samples.size(); ++s)
        {
            const float h = LoadF32(payload, s * sizeof(float));
            if (!std::isfinite(h))
            {
                TerrainResult<R> bad =
                    Fail<R>(Code::Corrupt, "tile {}: sample ({}, {}) is not finite", tile, s % side, s / side);
                bad.error.nonFiniteSample =
                    GlobalSampleIndex{index.layout.TileFirstSample(record.tile.x) + int64_t(s % side),
                                      index.layout.TileFirstSample(record.tile.z) + int64_t(s / side)};
                return bad;
            }
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            samples[s] = h;
        }
        if (lo != record.minHeight || hi != record.maxHeight)
            return Fail<R>(Code::Corrupt, "tile {}: heights span [{}, {}], the index says [{}, {}]", tile, lo, hi,
                           record.minHeight, record.maxHeight);

        std::optional<TileHeights> heights = TileHeights::Create(record.tile, intervals, std::move(samples));
        if (!heights)
            return Fail<R>(Code::Unsupported, "tile {}: {} intervals per tile is beyond this reader", tile, intervals);
        TerrainResult<TileHeights> result;
        result.value = std::move(heights);
        return result;
    }
}
