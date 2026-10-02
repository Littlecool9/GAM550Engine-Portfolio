/**
 * @file TerrainPackWriter.cpp
 * @author Jackey Cen
 * @brief Encodes a terrain release's index and packs in memory, and mints release ids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainPackWriter.h"

#include "LittleEndian.h"
#include "PackInternal.h"

#include <assets/AssetId.h>
#include <core/log/Assert.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <utility>

namespace mir
{
    namespace
    {
        using detail::Describe;
        using detail::Fail;
        using detail::LeWriter;

        std::vector<std::byte> EncodePayload(const TileHeights &tile)
        {
            std::vector<std::byte> bytes;
            bytes.reserve(tile.Samples().size() * sizeof(float));
            LeWriter out(bytes);
            for (float h : tile.Samples())
                out.F32(h);
            return bytes;
        }

        std::vector<const TileHeights *> SortedByZX(std::span<const TileHeights> tiles)
        {
            std::vector<const TileHeights *> sorted;
            sorted.reserve(tiles.size());
            for (const TileHeights &tile : tiles)
                sorted.push_back(&tile);
            std::sort(sorted.begin(), sorted.end(), [](const TileHeights *a, const TileHeights *b) {
                return detail::LessZX(a->Tile().x, a->Tile().z, b->Tile().x, b->Tile().z);
            });
            return sorted;
        }

        bool IsPowerOfTwo(uint32_t v)
        {
            return v != 0 && (v & (v - 1)) == 0;
        }
    }

    uint64_t TerrainHeightsHash(std::span<const TileHeights> tiles)
    {
        std::vector<TerrainTileRecord> records;
        records.reserve(tiles.size());
        for (const TileHeights *tile : SortedByZX(tiles))
        {
            TerrainTileRecord record;
            record.tile = tile->Tile();
            record.checksum = Fnv1a64(EncodePayload(*tile));
            records.push_back(record);
        }
        return detail::HeightsHashOf(records);
    }

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              const TerrainLayout &layout,
                                                              std::span<const TileHeights> tiles,
                                                              const TerrainPackWriteOptions &options)
    {
        using Code = TerrainPackErrorCode;
        using R = TerrainEncodedRelease;
        if (info.asset.IsNull() || info.release.IsNull())
            return Fail<R>(Code::InvalidInput, "asset and release ids must be set");
        if (info.format != kTerrainFormatVersion)
            return Fail<R>(Code::InvalidInput, "this writer encodes format {}, not {}", kTerrainFormatVersion.value,
                           info.format.value);
        if (options.chunkTiles < 1)
            return Fail<R>(Code::InvalidInput, "tiles per chunk {} is below 1", options.chunkTiles);
        if (!IsPowerOfTwo(options.payloadAlignment) || options.payloadAlignment > kMaxTerrainPayloadAlignment)
            return Fail<R>(Code::InvalidInput, "payload alignment {} is not a power of two up to {}",
                           options.payloadAlignment, kMaxTerrainPayloadAlignment);
        const int32_t n = layout.IntervalsPerTile();
        if (n > kMaxTerrainTileIntervals)
            return Fail<R>(Code::InvalidInput, "{} intervals per tile exceeds the format limit {}", n,
                           kMaxTerrainTileIntervals);

        const TileRange range = layout.Tiles();
        const uint64_t width = uint64_t(int64_t(range.max.x) - range.min.x + 1);
        const uint64_t height = uint64_t(int64_t(range.max.z) - range.min.z + 1);
        if (width > std::numeric_limits<uint32_t>::max() / height)
            return Fail<R>(Code::InvalidInput, "the layout has {} x {} tiles, more than one index can list", width,
                           height);

        const std::vector<const TileHeights *> sorted = SortedByZX(tiles);
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            const TileHeights &tile = *sorted[i];
            const std::string name = Describe(tile.Tile());
            if (!layout.Contains(tile.Tile()))
                return Fail<R>(Code::InvalidInput, "tile {} is outside the layout's tiles {} to {}", name,
                               Describe(range.min), Describe(range.max));
            if (i > 0 && sorted[i - 1]->Tile() == tile.Tile())
                return Fail<R>(Code::InvalidInput, "tile {} is given twice", name);
            if (tile.IntervalsPerTile() != n)
                return Fail<R>(Code::InvalidInput, "tile {} has {} intervals, the layout {}", name,
                               tile.IntervalsPerTile(), n);
            const std::span<const float> samples = tile.Samples();
            for (size_t s = 0; s < samples.size(); ++s)
            {
                if (!std::isfinite(samples[s]))
                    return Fail<R>(Code::InvalidInput, "tile {}: sample ({}, {}) is not finite", name,
                                   s % tile.SamplesPerSide(), s / tile.SamplesPerSide());
            }
        }
        // Sorted, unique and inside the layout, so a short list is missing a tile.
        for (uint64_t t = 0; t < width * height; ++t)
        {
            const TileCoord expected{static_cast<int32_t>(range.min.x + int64_t(t % width)),
                                     static_cast<int32_t>(range.min.z + int64_t(t / width))};
            if (t >= sorted.size() || sorted[t]->Tile() != expected)
                return Fail<R>(Code::InvalidInput, "tile {} is missing", Describe(expected));
        }

        // Record order is (z, x); a map keyed (z, x) gives packs the same order.
        std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> chunks;
        for (uint32_t t = 0; t < sorted.size(); ++t)
        {
            const TerrainChunkCoord chunk = TerrainChunkOf(sorted[t]->Tile(), options.chunkTiles);
            chunks[{chunk.z, chunk.x}].push_back(t);
        }

        TerrainEncodedRelease encoded;
        std::vector<TerrainTileRecord> records(sorted.size());
        std::vector<TerrainPackRecord> packs;
        packs.reserve(chunks.size());
        for (const auto &[key, members] : chunks)
        {
            const TerrainChunkCoord chunk{key.second, key.first};
            const uint32_t packIndex = static_cast<uint32_t>(packs.size());

            TerrainEncodedPack pack{chunk, TerrainPackFileName(chunk), {}};
            LeWriter out(pack.bytes);
            out.Bytes(detail::kPackMagic);
            out.U32(kTerrainFormatVersion.value);
            out.U32(kTerrainFormatMinor);
            out.U32(kTerrainPackHeaderSize);
            out.U64(info.asset.value);
            out.U64(info.release.value);
            out.I32(chunk.x);
            out.I32(chunk.z);
            out.U32(static_cast<uint32_t>(members.size()));
            out.U32(kTerrainHeightsContentVersion);
            MIR_ASSERT(out.Size() == kTerrainPackHeaderSize, "pack header layout drifted from the spec");

            for (uint32_t t : members)
            {
                const TileHeights &tile = *sorted[t];
                out.PadTo(options.payloadAlignment);
                const std::vector<std::byte> payload = EncodePayload(tile);
                const auto [lo, hi] = std::minmax_element(tile.Samples().begin(), tile.Samples().end());

                TerrainTileRecord &record = records[t];
                record.tile = tile.Tile();
                record.pack = packIndex;
                record.samplesPerSide = tile.SamplesPerSide();
                record.offset = out.Size();
                record.length = payload.size();
                record.checksum = Fnv1a64(payload);
                record.sampleSpacing = layout.SampleSpacing();
                record.minHeight = *lo;
                record.maxHeight = *hi;
                record.validExtent = *layout.ValidExtent(tile.Tile());
                out.Bytes(payload);
            }

            packs.push_back({chunk, static_cast<uint32_t>(members.size()), pack.bytes.size(), Fnv1a64(pack.bytes)});
            encoded.packs.push_back(std::move(pack));
        }

        encoded.heightsHash = detail::HeightsHashOf(records);
        const TerrainLocalBounds &bounds = layout.Bounds();
        LeWriter out(encoded.index);
        out.Bytes(detail::kIndexMagic);
        out.U32(kTerrainFormatVersion.value);
        out.U32(kTerrainFormatMinor);
        out.U32(kTerrainIndexHeaderSize);
        out.U32(kTerrainHeightsContentVersion);
        out.U32(options.payloadAlignment);
        out.U64(info.asset.value);
        out.U64(info.release.value);
        out.U64(info.source.value);
        out.F64(layout.SampleSpacing());
        out.I32(n);
        out.I32(options.chunkTiles);
        out.F64(bounds.minX);
        out.F64(bounds.minZ);
        out.F64(bounds.maxX);
        out.F64(bounds.maxZ);
        out.U32(static_cast<uint32_t>(packs.size()));
        out.U32(kTerrainPackRecordSize);
        out.U32(static_cast<uint32_t>(records.size()));
        out.U32(kTerrainTileRecordSize);
        out.U64(encoded.heightsHash);
        MIR_ASSERT(out.Size() == kTerrainIndexHeaderSize, "index header layout drifted from the spec");

        for (const TerrainPackRecord &pack : packs)
        {
            out.I32(pack.chunk.x);
            out.I32(pack.chunk.z);
            out.U32(pack.tileCount);
            out.U32(0);
            out.U64(pack.fileSize);
            out.U64(pack.checksum);
        }
        for (const TerrainTileRecord &r : records)
        {
            out.I32(r.tile.x);
            out.I32(r.tile.z);
            out.U32(r.pack);
            out.U32(static_cast<uint32_t>(r.samplesPerSide));
            out.U64(r.offset);
            out.U64(r.length);
            out.U64(r.checksum);
            out.F64(r.sampleSpacing);
            out.F32(r.minHeight);
            out.F32(r.maxHeight);
            out.F64(r.validExtent.minX);
            out.F64(r.validExtent.minZ);
            out.F64(r.validExtent.maxX);
            out.F64(r.validExtent.maxZ);
        }
        out.U64(Fnv1a64(encoded.index));

        TerrainResult<TerrainEncodedRelease> result;
        result.value = std::move(encoded);
        return result;
    }

    TerrainReleaseId MintTerrainReleaseId(AssetId asset, TerrainSourceFingerprint source, uint64_t heightsHash,
                                          uint64_t nonce)
    {
        std::vector<std::byte> bytes;
        LeWriter out(bytes);
        out.U64(asset.value);
        out.U64(source.value);
        out.U64(heightsHash);
        out.U64(nonce);
        const uint64_t id = Fnv1a64(bytes);
        // Zero means "no release"; 1 is as unlikely a collision as any other value.
        return TerrainReleaseId(id != 0 ? id : 1);
    }

    uint64_t MakeTerrainReleaseNonce()
    {
        std::random_device device;
        const uint64_t random = (uint64_t(device()) << 32) ^ device();
        const uint64_t now = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        return random ^ (now * 0x9E3779B97F4A7C15ull);
    }

    std::shared_ptr<TerrainMemorySource> MakeTerrainMemorySource(const TerrainEncodedRelease &release)
    {
        auto source = std::make_shared<TerrainMemorySource>();
        source->Put(std::string(kTerrainIndexFileName), release.index);
        for (const TerrainEncodedPack &pack : release.packs)
            source->Put(pack.fileName, pack.bytes);
        return source;
    }
}
