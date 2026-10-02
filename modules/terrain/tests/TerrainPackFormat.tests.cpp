/**
 * @file TerrainPackFormat.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainPackFormat and terrain/TerrainPackWriter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <string>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    // 256 m tiles at 4 m (65 x 65 samples). Tiles (-2..-1, -2..-1) all fall in
    // chunk (-1, -1), so the default 16-tile chunk gives one four-tile pack.
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
    // 120 header + 32 pack + 4 * 88 tiles + 8 trailer; 48 header + 4 payloads of 65^2 floats, 16-aligned.
    CHECK(encoded.index.size() == 512);
    CHECK(encoded.packs[0].bytes.size() == 48 + 3 * 16912 + 16900);

    auto [source, handle] = OpenInMemory(encoded);
    CHECK(handle.Info() == info);
    CHECK(handle.Index().formatMinor == kTerrainFormatMinor);
    CHECK(handle.Index().contentVersion == kTerrainHeightsContentVersion);
    CHECK(handle.Index().chunkTiles == kDefaultTerrainChunkTiles);
    CHECK(handle.Index().heightsHash == encoded.heightsHash);
    CHECK(handle.Index().heightsHash == TerrainHeightsHash(tiles));
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
    // Opening reads the index once, whole.
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

    // Input order does not matter either.
    std::vector<TileHeights> reversed(tiles.rbegin(), tiles.rend());
    CHECK(TerrainHeightsHash(reversed) == perTile.heightsHash);

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
    CHECK(TerrainHeightsHash(edited) != perTile.heightsHash);
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
    refused(info, missing, {}, "tile (0, 1) is missing");

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
    refused(info, nonFinite, {}, "sample (7, 9) is not finite");

    refused(Info(kNullAssetId, 0xE1), tiles, {}, "must be set");
    refused(Info(asset, 0), tiles, {}, "must be set");
    TerrainReleaseInfo future = info;
    future.format = TerrainFormatVersion(2);
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

    // Format 1.1 as a future writer might produce it: 16 more header bytes, 8
    // more per record, and an unknown section before the trailer.
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
    const uint64_t heights = TerrainHeightsHash(BuildTiles(EdgeLayout()));

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
