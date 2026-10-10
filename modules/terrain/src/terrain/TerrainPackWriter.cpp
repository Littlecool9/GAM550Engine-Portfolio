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
            r.maxDeviation = input.maxDeviation;r.sourceChecksum=input.sourceChecksum;
            r.checksum = Fnv1a64(EncodePayload(*input.heights));
            records.push_back(r);
        }
        std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
            return std::tuple(a.tile.z, a.tile.x, a.layer) < std::tuple(b.tile.z, b.tile.x, b.layer);
        });
        return detail::HeightsHashOf(records);
    }

    uint64_t TerrainPayloadsHash(std::span<const TerrainLayout> layers,std::span<const TerrainTileWrite> tiles,
                                std::span<const TerrainPayloadWrite> derived)
    {
        return TerrainPayloadsHash(layers, tiles, derived, TerrainRegionWrites{});
    }

    uint64_t TerrainPayloadsHash(std::span<const TerrainLayout> layers,std::span<const TerrainTileWrite> tiles,
                                std::span<const TerrainPayloadWrite> derived, const TerrainRegionWrites &regions)
    {
        if (layers.empty())
            return 0;
        std::vector<TerrainTileRecord> records;
        for(const auto &v:tiles)
        {
            if(!v.heights||v.layer>=layers.size())return 0;
            TerrainTileRecord r;r.tile=v.heights->Tile();r.layer=v.layer;r.source=v.source;
            r.sampleSpacing=layers[v.layer].SampleSpacing();r.samplesPerSide=v.heights->SamplesPerSide();
            r.maxDeviation=v.maxDeviation;r.sourceChecksum=v.sourceChecksum;r.checksum=Fnv1a64(EncodePayload(*v.heights));records.push_back(r);
        }
        for(const auto &v:derived){auto r=v.record;r.checksum=Fnv1a64(v.bytes);records.push_back(r);}
        std::sort(records.begin(),records.end(),[](const auto &a,const auto &b){
            return std::tuple(a.tile.z,a.tile.x,a.kind,a.level,a.layer)<std::tuple(b.tile.z,b.tile.x,b.kind,b.level,b.layer);});
        const auto ordered = detail::OrderRegionWrites(regions, layers.front());
        if (!ordered)
            return 0;
        std::vector<TerrainTileRecord> regionRecords;
        for (const TerrainPayloadWrite &v : *ordered.value)
            regionRecords.push_back(v.record);
        return detail::HeightsHashOf(records, regionRecords);
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
        return EncodeTerrainRelease(info,layers,tiles,{},options);
    }

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
        std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles,
        std::span<const TerrainPayloadWrite> derived, const TerrainPackWriteOptions &options)
    {
        return EncodeTerrainRelease(info, layers, tiles, derived, TerrainRegionWrites{}, options);
    }

    TerrainResult<TerrainEncodedRelease> EncodeTerrainRelease(const TerrainReleaseInfo &info,
        std::span<const TerrainLayout> layers, std::span<const TerrainTileWrite> tiles,
        std::span<const TerrainPayloadWrite> derived, const TerrainRegionWrites &regions,
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

        std::vector<TerrainPayloadWrite> inputs;
        for (const auto &v : sorted)
        {
            TerrainTileRecord r; r.tile=v.heights->Tile();r.layer=v.layer;r.source=v.source;r.maxDeviation=v.maxDeviation;r.sourceChecksum=v.sourceChecksum;
            r.samplesPerSide=v.heights->SamplesPerSide();r.sampleSpacing=layers[v.layer].SampleSpacing();
            auto [lo,hi]=std::minmax_element(v.heights->Samples().begin(),v.heights->Samples().end());
            r.minHeight=*lo;r.maxHeight=*hi;r.validExtent=*layout.ValidExtent(r.tile);
            inputs.push_back({r,EncodePayload(*v.heights)});
        }
        inputs.insert(inputs.end(),derived.begin(),derived.end());
        auto key=[](const auto &v) { const auto &r=v.record;return std::tuple(r.tile.z,r.tile.x,r.kind,r.level,r.layer); };
        std::sort(inputs.begin(),inputs.end(),[&](const auto &a,const auto &b){return key(a)<key(b);});
        if(inputs.size()>UINT32_MAX)return Fail<R>(Code::InvalidInput,"too many payloads");
        for(size_t i=0;i<inputs.size();++i)
        {
            auto &r=inputs[i].record;
            if(r.layer>=layers.size()||!layout.Contains(r.tile)||(i && key(inputs[i-1])==key(inputs[i])))
                return Fail<R>(Code::InvalidInput,"invalid or duplicate derived key");
            r.length=inputs[i].bytes.size();r.checksum=Fnv1a64(inputs[i].bytes);
        }

        TerrainResult<std::vector<TerrainPayloadWrite>> ordered = detail::OrderRegionWrites(regions, layout);
        if (!ordered)
            return detail::Failure<R>(ordered.error, "");
        const std::vector<TerrainPayloadWrite> &regionInputs = *ordered.value;

        // Regions go with their first map tile; their ids follow the tile payloads'.
        std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> chunks;
        for (uint32_t t = 0; t < inputs.size(); ++t)
        {
            const TerrainChunkCoord chunk = TerrainChunkOf(inputs[t].record.tile, options.chunkTiles);
            chunks[{chunk.z, chunk.x}].push_back(t);
        }
        for (uint32_t t = 0; t < regionInputs.size(); ++t)
        {
            const TerrainTileRecord &r = regionInputs[t].record;
            const TileCoord first = regions.grid->MapTiles({r.level, r.tile.x, r.tile.z}).min;
            const TerrainChunkCoord chunk = TerrainChunkOf(first, options.chunkTiles);
            chunks[{chunk.z, chunk.x}].push_back(static_cast<uint32_t>(inputs.size()) + t);
        }

        TerrainEncodedRelease encoded;
        std::vector<TerrainTileRecord> records(inputs.size());
        std::vector<TerrainTileRecord> regionRecords(regionInputs.size());
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
                out.PadTo(options.payloadAlignment);
                const bool region = t >= inputs.size();
                const TerrainPayloadWrite &input = region ? regionInputs[t - inputs.size()] : inputs[t];
                TerrainTileRecord &record = region ? regionRecords[t - inputs.size()] : records[t];
                record = input.record;
                record.pack=packIndex;record.offset=out.Size();
                out.Bytes(input.bytes);
            }

            packs.push_back({chunk, static_cast<uint32_t>(members.size()), pack.bytes.size(), Fnv1a64(pack.bytes)});
            encoded.packs.push_back(std::move(pack));
        }

        encoded.heightsHash = detail::HeightsHashOf(records, regionRecords);
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
        out.U32(static_cast<uint32_t>(regionRecords.size()));
        out.U32(kTerrainTileRecordSize);
        out.U32(regions.grid ? regions.grid->RegionTiles() : 0);
        out.U32(regions.grid ? regions.grid->Cells() : 0);
        out.U32(regions.grid ? regions.grid->Unit() : 0);
        out.U32(regions.grid ? regions.grid->Levels() : 0);
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
        auto writeRecord = [&](const TerrainTileRecord &r) {
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
            out.U64(r.sourceChecksum);out.U64(r.topologyHash);out.U32(r.vertexCount);out.U32(r.indexCount);
        };
        for (const TerrainTileRecord &r : records)
            writeRecord(r);
        for (const auto &layer : layers)
        {
            out.F64(layer.SampleSpacing());
            out.I32(layer.IntervalsPerTile());
            out.U32(0);
        }
        for (const TerrainTileRecord &r : regionRecords)
            writeRecord(r);
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
