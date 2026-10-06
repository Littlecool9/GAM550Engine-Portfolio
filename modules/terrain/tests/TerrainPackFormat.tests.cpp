/**
 * @file TerrainPackFormat.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainPackFormat and terrain/TerrainPackWriter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include <terrain/TerrainHeightQuery.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <terrain/TerrainSurface.h>
#include <limits>
#include <string>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    // 256 m tiles at 4 m; tiles (-2..-1, -2..-1) all fall in chunk (-1, -1).
    TerrainLayout PrototypeLayout()
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-512.0, -512.0, 0.0, 0.0});
        REQUIRE(layout);
        return *layout;
    }
}

TEST_CASE("A release round trips every tile through its index and pack", "[terrain][pack]")
{
    const TerrainLayout layout = PrototypeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const AssetId asset = TerrainAsset("terrain/proto.terrain.json");
    const TerrainReleaseInfo info = Info(asset, 0xA1);

    const TerrainEncodedRelease encoded = Encode(info, layout, tiles);
    REQUIRE(encoded.packs.size() == 1);
    CHECK(encoded.packs[0].fileName == "chunk_-1_-1.tpack");
    // 128 header + 32 pack + 4 * 120 payloads + 16 layer + 8 trailer; 48 header + 4 payloads of 65^2 floats, 16-aligned.
    CHECK(encoded.index.size() == 664);
    CHECK(encoded.packs[0].bytes.size() == 48 + 3 * 16912 + 16900);

    auto [source, handle] = OpenInMemory(encoded);
    CHECK(handle.Info() == info);
    CHECK(handle.Index().formatMinor == kTerrainFormatMinor);
    CHECK(handle.Index().contentVersion == kTerrainHeightsContentVersion);
    CHECK(handle.Index().chunkTiles == kDefaultTerrainChunkTiles);
    CHECK(handle.Index().heightsHash == encoded.heightsHash);
    CHECK(handle.Index().heightsHash == TerrainHeightsHash(layout, tiles));
    CHECK(handle.Layout().SampleSpacing() == 4.0);
    CHECK(handle.Layout().IntervalsPerTile() == 64);

    for (const TileHeights &tile : tiles)
    {
        CHECK(ReadOk(handle, tile.Tile()) == tile);
        const TerrainTileRecord *record = handle.Index().Find(tile.Tile());
        REQUIRE(record);
        const auto [lo, hi] = std::minmax_element(tile.Samples().begin(), tile.Samples().end());
        CHECK(record->minHeight == *lo);
        CHECK(record->maxHeight == *hi);
        CHECK(record->sampleSpacing == 4.0);
        CHECK(record->offset % kDefaultTerrainPayloadAlignment == 0);
    }
    CHECK(handle.VerifyAllPacks().code == Code::None);
}

TEST_CASE("Reading one tile of a multi-tile pack touches only its recorded range", "[terrain][pack]")
{
    const TerrainLayout layout = PrototypeLayout();
    const AssetId asset = TerrainAsset("terrain/proto.terrain.json");
    const TerrainEncodedRelease encoded = Encode(Info(asset, 0xA1), layout, BuildTiles(layout));

    auto [source, handle] = OpenInMemory(encoded);
    CHECK(source->Stats().reads == 1);
    CHECK(source->Stats().bytes == encoded.index.size());

    source->ResetStats();
    source->ClearRequests();
    const TileCoord coord{-1, -2};
    CHECK(ReadOk(handle, coord).Tile() == coord);

    const TerrainTileRecord *record = handle.Index().Find(coord);
    REQUIRE(record);
    const std::vector<TerrainMemorySource::Request> requests = source->Requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].file == "chunk_-1_-1.tpack");
    CHECK(requests[0].offset == record->offset);
    CHECK(requests[0].length == record->length);
    CHECK(record->length == 65 * 65 * sizeof(float));
    CHECK(source->Stats().reads == 1);
    CHECK(source->Stats().bytes == 16900);
    CHECK(encoded.packs[0].bytes.size() == 67684);
}

TEST_CASE("Negative coordinates and clipped map-edge tiles round trip", "[terrain][pack]")
{
    const TerrainLayout layout = EdgeLayout();
    REQUIRE(layout.Tiles().min == TileCoord{-2, -1});
    REQUIRE(layout.Tiles().max == TileCoord{0, 1});
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainEncodedRelease encoded = EncodeEdge(asset, 0xE1);

    REQUIRE(encoded.packs.size() == 4);
    CHECK(encoded.packs[0].fileName == "chunk_-1_-1.tpack");
    CHECK(encoded.packs[1].fileName == "chunk_0_-1.tpack");
    CHECK(encoded.packs[2].fileName == "chunk_-1_0.tpack");
    CHECK(encoded.packs[3].fileName == "chunk_0_0.tpack");

    auto [source, handle] = OpenInMemory(encoded);
    CHECK(handle.Index().packs[2].tileCount == 4);

    const TerrainTileRecord *corner = handle.Index().Find({-2, -1});
    REQUIRE(corner);
    CHECK(corner->validExtent.minX == -300.5);
    CHECK(corner->validExtent.minZ == -10.0);
    CHECK(corner->validExtent.maxX == -256.0);
    CHECK(corner->validExtent.maxZ == 0.0);

    const TerrainTileRecord *far = handle.Index().Find({0, 1});
    REQUIRE(far);
    CHECK(far->validExtent.minX == 0.0);
    CHECK(far->validExtent.minZ == 256.0);
    CHECK(far->validExtent.maxX == 100.25);
    CHECK(far->validExtent.maxZ == 260.0);

    const TerrainTileRecord *inner = handle.Index().Find({-1, 0});
    REQUIRE(inner);
    CHECK(inner->validExtent.minX == -256.0);
    CHECK(inner->validExtent.maxZ == 256.0);

    for (const TileHeights &tile : tiles)
        CHECK(ReadOk(handle, tile.Tile()) == tile);

    // Padding samples outside the bounds are stored like any other.
    const TileHeights cornerTile = ReadOk(handle, {-2, -1});
    CHECK(layout.IsPaddingSample({-128, -64}));
    CHECK(cornerTile.At(0, 0) == AnalyticHeight(-128, -64));

    // Neighbours hold identical copies of their shared edge.
    const TileHeights left = ReadOk(handle, {-2, 0});
    const TileHeights right = ReadOk(handle, {-1, 0});
    for (int32_t k = 0; k <= 64; ++k)
        CHECK(left.At(64, k) == right.At(0, k));
}

TEST_CASE("Tile addresses and the heights hash do not depend on chunk size or alignment", "[terrain][pack]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");

    const TerrainEncodedRelease perTile = Encode(Info(asset, 0xE1), layout, tiles, {.chunkTiles = 1});
    const TerrainEncodedRelease paged = Encode(Info(asset, 0xE1), layout, tiles,
                                               {.chunkTiles = 16, .payloadAlignment = 4096});
    CHECK(perTile.packs.size() == 9);
    CHECK(paged.packs.size() == 4);
    CHECK(perTile.heightsHash == paged.heightsHash);

    std::vector<TileHeights> reversed(tiles.rbegin(), tiles.rend());
    CHECK(TerrainHeightsHash(layout, reversed) == perTile.heightsHash);

    auto [perTileSource, perTileHandle] = OpenInMemory(perTile);
    auto [pagedSource, pagedHandle] = OpenInMemory(paged);
    CHECK(pagedHandle.Index().payloadAlignment == 4096);
    for (const TileHeights &tile : tiles)
    {
        CHECK(ReadOk(perTileHandle, tile.Tile()) == tile);
        CHECK(ReadOk(pagedHandle, tile.Tile()) == tile);
        CHECK(pagedHandle.Index().Find(tile.Tile())->offset % 4096 == 0);
    }

    std::vector<TileHeights> edited = tiles;
    edited[4].Set(3, 3, edited[4].At(3, 3) + 0.5f);
    CHECK(TerrainHeightsHash(layout, edited) != perTile.heightsHash);
}

TEST_CASE("The writer refuses incomplete, duplicate or invalid tiles", "[terrain][pack]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainReleaseInfo info = Info(asset, 0xE1);

    auto refused = [&](const TerrainReleaseInfo &i, std::span<const TileHeights> t, TerrainPackWriteOptions o,
                       std::string_view text) {
        const TerrainResult<TerrainEncodedRelease> result = EncodeTerrainRelease(i, layout, t, o);
        CHECK_FALSE(result.value.has_value());
        CHECK(result.error.code == Code::InvalidInput);
        CHECK(Mentions(result.error, text));
    };

    std::vector<TileHeights> missing(tiles.begin(), tiles.end() - 1);
    refused(info, missing, {}, "source tile is missing");

    std::vector<TileHeights> duplicate = tiles;
    duplicate.push_back(tiles[2]);
    refused(info, duplicate, {}, "given twice");

    std::vector<TileHeights> outside = tiles;
    outside.push_back(*TileHeights::Create({5, 5}, 64));
    refused(info, outside, {}, "outside");

    std::vector<TileHeights> coarse = tiles;
    coarse[0] = *TileHeights::Create(tiles[0].Tile(), 32);
    refused(info, coarse, {}, "32 intervals");

    std::vector<TileHeights> nonFinite = tiles;
    nonFinite[3].Set(7, 9, std::numeric_limits<float>::quiet_NaN());
    refused(info, nonFinite, {}, "sample is not finite");

    refused(Info(kNullAssetId, 0xE1), tiles, {}, "must be set");
    refused(Info(asset, 0), tiles, {}, "must be set");
    TerrainReleaseInfo future = info;
    future.format = TerrainFormatVersion(3);
    refused(future, tiles, {}, "format");
    refused(info, tiles, {.chunkTiles = 0}, "tiles per chunk");
    refused(info, tiles, {.payloadAlignment = 24}, "alignment");
    refused(info, tiles, {.payloadAlignment = 1u << 17}, "alignment");
}

TEST_CASE("A newer minor with longer header, records and an extension section still reads", "[terrain][pack]")
{
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainEncodedRelease encoded = EncodeEdge(asset, 0xE1);
    const std::span<const std::byte> v10(encoded.index);
    const uint32_t packCount = 4;
    const uint32_t tileCount = 9;

    // Format 2.1: a longer header and records, and an unknown section before the trailer.
    std::vector<std::byte> v11(v10.begin(), v10.begin() + kTerrainIndexHeaderSize);
    v11.insert(v11.end(), 16, std::byte{0xAB});
    StoreU32(v11, 8, 1);
    StoreU32(v11, 12, kTerrainIndexHeaderSize + 16);
    StoreU32(v11, 100, kTerrainPackRecordSize + 8);
    StoreU32(v11, 108, kTerrainTileRecordSize + 8);
    for (uint32_t p = 0; p < packCount; ++p)
    {
        const auto at = v10.begin() + kTerrainIndexHeaderSize + p * kTerrainPackRecordSize;
        v11.insert(v11.end(), at, at + kTerrainPackRecordSize);
        v11.insert(v11.end(), 8, std::byte{0xCD});
    }
    for (uint32_t t = 0; t < tileCount; ++t)
    {
        const auto at = v10.begin() + TileRecordAt(packCount, t);
        v11.insert(v11.end(), at, at + kTerrainTileRecordSize);
        v11.insert(v11.end(), 8, std::byte{0xEF});
    }
    const auto layerAt = v10.begin() + TileRecordAt(packCount, tileCount);
    v11.insert(v11.end(), layerAt, layerAt + kTerrainLayerRecordSize);
    v11.insert(v11.end(), 24, std::byte{0x11});
    v11.insert(v11.end(), kTerrainIndexTrailerSize, std::byte{0});
    ResealIndex(v11);

    TerrainEncodedRelease newer = encoded;
    newer.index = v11;
    auto [source, handle] = OpenInMemory(newer);
    auto [baseSource, base] = OpenInMemory(encoded);
    CHECK(handle.Index().formatMinor == 1);
    CHECK(handle.Info() == base.Info());
    CHECK(handle.Index().heightsHash == base.Index().heightsHash);
    for (uint32_t t = 0; t < tileCount; ++t)
    {
        const TerrainTileRecord &a = handle.Index().tiles[t];
        const TerrainTileRecord &b = base.Index().tiles[t];
        CHECK(a.tile == b.tile);
        CHECK(a.offset == b.offset);
        CHECK(a.checksum == b.checksum);
        CHECK(ReadOk(handle, a.tile) == ReadOk(base, b.tile));
    }
    CHECK(handle.VerifyAllPacks().code == Code::None);
}

TEST_CASE("Release ids are minted per bake and never zero", "[terrain][pack]")
{
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainSourceFingerprint source(0x5eed);
    const uint64_t heights = TerrainHeightsHash(EdgeLayout(), BuildTiles(EdgeLayout()));

    const TerrainReleaseId a = MintTerrainReleaseId(asset, source, heights, 1);
    CHECK_FALSE(a.IsNull());
    CHECK(MintTerrainReleaseId(asset, source, heights, 1) == a);
    CHECK(MintTerrainReleaseId(asset, source, heights, 2) != a);
    CHECK(MintTerrainReleaseId(asset, TerrainSourceFingerprint(0x5eee), heights, 1) != a);
    CHECK(MintTerrainReleaseId(asset, source, heights + 1, 1) != a);
    CHECK(MintTerrainReleaseId(TerrainAsset("terrain/other.terrain.json"), source, heights, 1) != a);

    // Two bakes of the same source and heights are still two releases.
    const uint64_t first = MakeTerrainReleaseNonce();
    const uint64_t second = MakeTerrainReleaseNonce();
    CHECK(first != second);
    CHECK(MintTerrainReleaseId(asset, source, heights, first) != MintTerrainReleaseId(asset, source, heights, second));
}

namespace
{
    struct MixedRelease
    {
        TerrainReleaseInfo info = Info(TerrainAsset("terrain/mixed.terrain.json"), 0x145);
        std::vector<TerrainLayout> layers;
        std::vector<std::vector<TileHeights>> heights;
        std::vector<TerrainTileWrite> records;

        MixedRelease()
        {
            for (double spacing : {1.0, 2.0, 4.0, 8.0})
            {
                layers.push_back(*TerrainLayout::CreateFromTileSize(16.0, spacing, {-16, -16, 16, 16}));
                heights.push_back(BuildTiles(layers.back()));
            }
            for (uint32_t l = 0; l < 4; ++l)
                for (uint32_t t = 0; t < 4; ++t)
                    if (l == t || l == 3)
                        records.push_back({&heights[l][t], l, l == t, l == t ? std::nullopt : std::optional(100.0)});
        }

        TerrainEncodedRelease Encode() const
        {
            auto encoded = EncodeTerrainRelease(info, layers, records);
            INFO(encoded.error.message);
            REQUIRE(encoded);
            return std::move(*encoded.value);
        }
    };
}

TEST_CASE("Format 2 sparse layers preserve independent ranges and tile source precision", "[terrain][pack][layers]")
{
    const MixedRelease fixture;
    const auto encoded = fixture.Encode();
    auto [source, handle] = OpenInMemory(encoded);
    REQUIRE(handle.Layers().size() == 4);
    CHECK(handle.Index().tiles.size() == 7);
    CHECK(handle.VerifyAllPacks().code == Code::None);
    CHECK(encoded.heightsHash == TerrainHeightsHash(fixture.layers, fixture.records));
    for (const auto &r : handle.Index().tiles)
    {
        source->ResetStats(); source->ClearRequests();
        auto read = handle.ReadTile({fixture.info.asset, r.tile}, r.layer);
        REQUIRE(read);
        CHECK(read.value->SamplesPerSide() == r.samplesPerSide);
        REQUIRE(source->Requests().size() == 1);
        CHECK(source->Requests()[0].offset == r.offset);
        CHECK(source->Requests()[0].length == r.length);
    }
    CHECK_FALSE(handle.ReadTile({fixture.info.asset, {0, 0}}, 0));
    auto query = TerrainHeightQuery::Create(handle);
    const auto preload = PreloadAllTerrainTiles(query, handle, 3);
    REQUIRE(preload.Ok());
    CHECK(preload.loaded == 4);
    source->ResetStats();
    for (uint32_t t = 0; t < 4; ++t)
    {
        const TileCoord tile = fixture.heights[0][t].Tile();
        const auto position = *TerrainLocalPosition::FromMeters({tile.x * 16.0 + 3, 0, tile.z * 16.0 + 3});
        const auto r = query.HeightAt(position, {fixture.info, 1.0});
        CHECK(r.sourceSpacing == fixture.layers[t].SampleSpacing());
        CHECK(r.spacing == 8.0);
        CHECK(r.status == (t == 0 ? TerrainHeightStatus::CoarserWhileLoading : TerrainHeightStatus::CoarserAtSourceLimit));
        if (t < 3) CHECK(r.maxError == 100.0 + SurfaceTolerance(
            std::max(std::abs(double(handle.Index().Find(tile, 3)->minHeight)), std::abs(double(handle.Index().Find(tile, 3)->maxHeight)))));
    }
    CHECK(source->Stats().reads == 0);
    CHECK(PreloadAllTerrainTiles(query, handle).loaded == 7);
    for (uint32_t t = 0; t < 4; ++t)
    {
        const auto tile = fixture.heights[0][t].Tile();
        const auto position = *TerrainLocalPosition::FromMeters({tile.x * 16.0 + 3, 0, tile.z * 16.0 + 3});
        const auto r = query.HeightAt(position, {fixture.info, fixture.layers[t].SampleSpacing()});
        CHECK(r.status == TerrainHeightStatus::MeetsRequirement);
        CHECK(r.spacing == r.sourceSpacing);
    }
}

TEST_CASE("Format 2 rejects missing duplicate unknown and non-nesting layer records", "[terrain][pack][layers]")
{
    MixedRelease fixture;
    const auto encoded = fixture.Encode();
    const auto index = *ParseTerrainIndex(encoded.index).value;
    const size_t table = kTerrainIndexHeaderSize + index.packs.size() * kTerrainPackRecordSize;
    auto refused = [&](auto patch, std::string_view field) {
        auto bytes = encoded.index;
        patch(bytes);
        ResealIndex(bytes);
        const auto parsed = ParseTerrainIndex(bytes);
        CHECK_FALSE(parsed);
        CHECK(parsed.error.code == Code::Malformed);
        CHECK(Mentions(parsed.error, field));
    };
    refused([&](auto &b) { StoreU32(b, table + 100, 0); }, "no source record");
    refused([&](auto &b) { StoreU32(b, table + kTerrainTileRecordSize + 100, 1); }, "two source records");
    refused([&](auto &b) { StoreU32(b, table + 88, 99); }, "unknown layer");
    refused([&](auto &b) { StoreU64(b, table + 24, 4); }, "payload length");
    const size_t layerTable = table + index.tiles.size() * kTerrainTileRecordSize;
    refused([&](auto &b) { StoreU64(b, layerTable + kTerrainLayerRecordSize, std::bit_cast<uint64_t>(3.0)); }, "does not nest");
    refused([&](auto &b) { StoreU64(b, table + kTerrainTileRecordSize + 16, LoadU64(b, table + 16)); }, "overlap");
    auto legacy = encoded.index;
    StoreU32(legacy, 4, 1);
    CHECK(ParseTerrainIndex(legacy).error.code == Code::Unsupported);
    fixture.records[0].source = false;
    CHECK(EncodeTerrainRelease(fixture.info, fixture.layers, fixture.records).error.code == Code::InvalidInput);
}

TEST_CASE("Per-tile query error bounds reject invalid values and unbaked coordinates", "[terrain][query][layers]")
{
    const auto layout = *TerrainLayout::CreateFromTileSize(16.0, 1.0, {0, 0, 32, 16});
    TerrainHeightLayer layer{layout, std::vector<TileCoord>{{0, 0}, {1, 0}}, std::nullopt};
    const auto info = Info(TerrainAsset("terrain/bounds.terrain.json"), 0x146);
    for (double bound : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        layer.tileDeviations = {{{0, 0}, bound}};
        CHECK_FALSE(TerrainHeightQuery::Create(info, {layer}));
    }
    layer.tileDeviations = {{{2, 0}, 1.0}};
    CHECK_FALSE(TerrainHeightQuery::Create(info, {layer}));
    const auto coarse = *TerrainLayout::CreateFromTileSize(16.0, 2.0, {0, 0, 32, 16});
    layer.bakedTiles = std::vector<TileCoord>{{0, 0}};
    layer.tileDeviations = {{{1, 0}, 1.0}};
    CHECK_FALSE(TerrainHeightQuery::Create(info, {layer, {coarse}}));

    const MixedRelease fixture;
    auto [source, handle] = OpenInMemory(fixture.Encode());
    auto query = TerrainHeightQuery::Create(handle);
    const std::vector<TileCoord> requested{{-1, -1}, {-1, -1}, {0, 0}};
    source->ClearRequests();
    const auto report = PreloadTerrainTiles(query, handle, requested, 0);
    CHECK(report.loaded == 1);
    CHECK(report.failed == 1);
    CHECK(report.firstError.code == TerrainPackErrorCode::NotFound);
    CHECK(source->Requests().size() == 1);
    CHECK(query.Residency(0, {0, 0}) == TerrainTileResidency::Absent);
    CHECK(PreloadAllTerrainTiles(query, handle, 99).firstError.code == TerrainPackErrorCode::InvalidInput);
}
