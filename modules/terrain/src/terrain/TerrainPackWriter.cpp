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
#include <tuple>

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

        bool IsPowerOfTwo(uint32_t v)
        {
            return v != 0 && (v & (v - 1)) == 0;
        }
    }

    uint64_t TerrainHeightsHash(std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles)
    {
        std::vector<TerrainTileRecord> records;
        for (const auto &input : tiles)
        {
            if (!input.heights || input.layer >= layers.size()) return 0;
            TerrainTileRecord r;
            r.tile = input.heights->Tile(); r.layer = input.layer; r.source = input.source;
            r.sampleSpacing = layers[input.layer].SampleSpacing();
            r.samplesPerSide = input.heights->SamplesPerSide();
            r.maxDeviation = input.maxDeviation;
            r.checksum = Fnv1a64(EncodePayload(*input.heights));
            records.push_back(r);
        }
        std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
            return std::tuple(a.tile.z, a.tile.x, a.layer) < std::tuple(b.tile.z, b.tile.x, b.layer);
        });
        return detail::HeightsHashOf(records);
    }

    uint64_t TerrainHeightsHash(const TerrainLayout &layout, std::span<const TileHeights> tiles)
    {
        std::vector<TerrainTileWrite> inputs;
        for (const auto &tile : tiles) inputs.push_back({&tile});
        return TerrainHeightsHash(std::span(&layout, 1), inputs);
    }

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              const TerrainLayout &layout,
                                                              std::span<const TileHeights> tiles,
                                                              const TerrainPackWriteOptions &options)
    {
        std::vector<TerrainTileWrite> inputs;
        for (const auto &tile : tiles) inputs.push_back({&tile});
        return EncodeTerrainRelease(info, std::span(&layout, 1), inputs, options);
    }

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
                                                              std::span<const TerrainLayout> layers,
                                                              std::span<const TerrainTileWrite> tiles,
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
        if (layers.empty() || layers.size() > UINT32_MAX || tiles.size() > UINT32_MAX)
            return Fail<R>(Code::InvalidInput, "empty or oversized layer/payload tables");
        const TerrainLayout &layout = layers.front();
        const int32_t n = layout.IntervalsPerTile();
        for (size_t l = 0; l < layers.size(); ++l)
        {
            if (layers[l].IntervalsPerTile() > kMaxTerrainTileIntervals ||
                (l > 0 && !TerrainLayersNest(layers[l - 1], layers[l])))
                return Fail<R>(Code::InvalidInput, "layer {} spacing does not nest or exceeds the tile limit", l);
        }
        std::vector<TerrainTileWrite> sorted(tiles.begin(), tiles.end());
        for (const auto &input : sorted)
        {
            if (!input.heights || input.layer >= layers.size())
                return Fail<R>(Code::InvalidInput, "null tile or unknown layer {}", input.layer);
            const auto &tile = *input.heights;
            const auto &layer = layers[input.layer];
            if (!layer.Contains(tile.Tile()))
                return Fail<R>(Code::InvalidInput, "tile {} is outside the layout", Describe(tile.Tile()));
            if (tile.IntervalsPerTile() != layer.IntervalsPerTile())
                return Fail<R>(Code::InvalidInput, "tile {} has {} intervals, the layer {}", Describe(tile.Tile()),
                               tile.IntervalsPerTile(), layer.IntervalsPerTile());
            for (float h : tile.Samples())
                if (!std::isfinite(h))
                    return Fail<R>(Code::InvalidInput, "tile {}: sample is not finite", Describe(tile.Tile()));
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return std::tuple(a.heights->Tile().z, a.heights->Tile().x, a.layer) <
                   std::tuple(b.heights->Tile().z, b.heights->Tile().x, b.layer);
        });
        for (size_t t = 1; t < sorted.size(); ++t)
            if (sorted[t - 1].heights->Tile() == sorted[t].heights->Tile() && sorted[t - 1].layer == sorted[t].layer)
                return Fail<R>(Code::InvalidInput, "tile {} layer {} is given twice", Describe(sorted[t].heights->Tile()),
                               sorted[t].layer);

        std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> chunks;
        for (uint32_t t = 0; t < sorted.size(); ++t)
        {
            const TerrainChunkCoord chunk = TerrainChunkOf(sorted[t].heights->Tile(), options.chunkTiles);
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
                const TileHeights &tile = *sorted[t].heights;
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
                record.layer = sorted[t].layer;
                record.source = sorted[t].source;
                record.maxDeviation = sorted[t].maxDeviation;
                record.sampleSpacing = layers[record.layer].SampleSpacing();
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
        out.U32(static_cast<uint32_t>(layers.size()));
        out.U32(kTerrainLayerRecordSize);
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
            out.U32(r.layer);
            out.U32(static_cast<uint32_t>(r.kind));
            out.U32(r.level);
            out.U32(r.source ? 1 : 0);
            out.U32(r.maxDeviation ? 1 : 0);
            out.U32(0);
            out.F64(r.maxDeviation.value_or(0.0));
        }
        for (const auto &layer : layers)
        {
            out.F64(layer.SampleSpacing());
            out.I32(layer.IntervalsPerTile());
            out.U32(0);
        }
        out.U64(Fnv1a64(encoded.index));
        const auto checked = ParseTerrainIndex(encoded.index);
        if (!checked)
            return detail::Failure<R>(Code::InvalidInput, checked.error.message);

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
