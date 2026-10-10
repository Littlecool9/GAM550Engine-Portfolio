/**
 * @file TerrainRegion.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainRegion.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainRegion.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <string>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    TerrainRegionGrid Grid(const TerrainLayout &layout, uint32_t regionTiles, uint32_t cells, uint32_t unit = 1)
    {
        const std::optional<TerrainRegionGrid> grid = TerrainRegionGrid::Create(layout, regionTiles, cells, unit);
        REQUIRE(grid);
        return *grid;
    }

    TileSampleLookup LookupOf(const std::vector<TileHeights> &tiles)
    {
        return [&tiles](TileCoord coord) -> std::span<const float> {
            for (const TileHeights &tile : tiles)
                if (tile.Tile() == coord)
                    return tile.Samples();
            return {};
        };
    }

    struct RegionRelease
    {
        TerrainLayout layout;
        std::vector<TileHeights> tiles;
        TerrainReleaseInfo info;
        TerrainRegionWrites regions;
        TerrainEncodedRelease encoded;

        RegionRelease(TerrainLayout l, uint32_t regionTiles, uint32_t cells, TerrainPackWriteOptions options = {})
            : layout(l), tiles(BuildTiles(l)), info(Info(TerrainAsset("terrain/regions.terrain.json"), 0x124)),
              regions(BuildTestRegions(l, tiles, regionTiles, cells)),
              encoded(EncodeWithRegions(info, l, tiles, regions, options))
        {
        }
    };

    bool SameMesh(const TerrainRegionMesh &a, const TerrainRegionMesh &b)
    {
        return a.region == b.region && a.tiles.min == b.tiles.min && a.tiles.max == b.tiles.max &&
               a.cellSpacing == b.cellSpacing && a.sourceChecksum == b.sourceChecksum &&
               a.topologyHash == b.topologyHash && a.maxDeviation == b.maxDeviation &&
               a.origin.Meters() == b.origin.Meters() && a.positions == b.positions && a.normals == b.normals &&
               a.indices == b.indices;
    }

    TerrainRegionMesh DecodeWritten(const RegionRelease &r, size_t ordinal)
    {
        const TerrainIndex index = *ParseTerrainIndex(r.encoded.index).value;
        const TerrainResult<TerrainRegionMesh> mesh =
            DecodeTerrainRegion(index, index.regions[ordinal], r.regions.payloads[ordinal].bytes);
        INFO(mesh.error.message);
        REQUIRE(mesh);
        return *mesh.value;
    }
}

TEST_CASE("Level zero Regions sit on the tile-zero grid and each level doubles until one covers the map",
          "[terrain][region]")
{
    const TerrainRegionGrid straddling = Grid(ReliefLayout(), 16, 8);
    CHECK(straddling.FirstTile() == TileCoord{-16, -16});
    REQUIRE(straddling.Levels() == 2);
    CHECK(straddling.Count(0) == TileCoord{2, 2});
    CHECK(straddling.Count(1) == TileCoord{1, 1});
    CHECK(straddling.RegionCount() == 5);
    CHECK(straddling.Tiles({0, 0, 0}).min == TileCoord{-16, -16});
    CHECK(straddling.Tiles({0, 0, 0}).max == TileCoord{-1, -1});
    CHECK(straddling.MapTiles({0, 1, 0}).min == TileCoord{0, -1});
    CHECK(straddling.MapTiles({0, 1, 0}).max == TileCoord{0, -1});
    CHECK(straddling.MapTiles({1, 0, 0}).min == TileCoord{-1, -1});
    CHECK(straddling.MapTiles({1, 0, 0}).max == TileCoord{0, 0});
    CHECK(straddling.Rect({0, 1, 1}) == TerrainLocalBounds{0.0, 0.0, 4096.0, 4096.0});
    CHECK(straddling.ValidExtent({0, 1, 1}) == TerrainLocalBounds{0.0, 0.0, 256.0, 250.0});
    CHECK(straddling.ValidExtent({1, 0, 0}) == ReliefLayout().Bounds());

    const auto inside = TerrainLayout::CreateFromTileSize(256.0, 4.0, {10.0, 20.0, 500.0, 300.0});
    REQUIRE(inside);
    const TerrainRegionGrid single = Grid(*inside, 16, 8);
    CHECK(single.Levels() == 1);
    CHECK(single.RegionCount() == 1);
    CHECK(single.ValidExtent({0, 0, 0}) == inside->Bounds());

    const TerrainRegionGrid tiles = Grid(EdgeLayout(), 1, 8);
    REQUIRE(tiles.Levels() == 3);
    CHECK(tiles.Count(0) == TileCoord{3, 3});
    CHECK(tiles.Count(1) == TileCoord{2, 2});
    CHECK(tiles.Count(2) == TileCoord{1, 1});
    CHECK(tiles.RegionCount() == 14);
    for (uint64_t ordinal = 0; ordinal < tiles.RegionCount(); ++ordinal)
    {
        const TerrainRegionCoord region = tiles.AtOrdinal(ordinal);
        CHECK(tiles.Contains(region));
        CHECK(tiles.Ordinal(region) == ordinal);
        if (const auto parent = tiles.Parent(region))
        {
            const std::vector<TerrainRegionCoord> siblings = tiles.Children(*parent);
            CHECK(std::find(siblings.begin(), siblings.end(), region) != siblings.end());
        }
        else
            CHECK(region.level == tiles.Levels() - 1);
        for (const TerrainRegionCoord child : tiles.Children(region))
            CHECK(tiles.Parent(child) == region);
    }
    CHECK(tiles.Children({1, 1, 1}).size() == 1);
    CHECK(tiles.Containing(0, {-2, 1}) == TerrainRegionCoord{0, 0, 2});
    CHECK(tiles.Containing(1, {0, 1}) == TerrainRegionCoord{1, 1, 1});
    CHECK_FALSE(tiles.Contains({0, 3, 0}));
    CHECK_FALSE(tiles.Contains({3, 0, 0}));
}

TEST_CASE("Region grid lines are unit multiples and the domain stops at the cells touching the map",
          "[terrain][region]")
{
    const TerrainLayout layout = EdgeLayout();
    const TerrainRegionGrid grid = Grid(layout, 1, 8);
    const SampleRange corner = grid.Domain({0, 0, 0});
    CHECK(corner.min == GlobalSampleIndex{-76, -3});
    CHECK(corner.max == GlobalSampleIndex{-64, 0});
    CHECK(grid.LinesX({0, 0, 0}) == std::vector<int64_t>{-76, -72, -64});
    CHECK(grid.LinesZ({0, 0, 0}) == std::vector<int64_t>{-3, 0});
    CHECK(grid.ValidExtent({0, 0, 0}) == TerrainLocalBounds{-300.5, -10.0, -256.0, 0.0});
    CHECK(grid.CellSpacing(0) == 32.0);
    CHECK(grid.SamplesPerSide(0) == 9);
    CHECK(grid.CellSpacing(2) == 128.0);

    const std::vector<int64_t> middle = grid.LinesX({0, 1, 1});
    REQUIRE(middle.size() == 9);
    for (size_t i = 0; i < middle.size(); ++i)
        CHECK(middle[i] == -64 + int64_t(i) * 8);

    const TerrainRegionGrid coarse = Grid(layout, 1, 8, 16);
    CHECK(coarse.LinesX({0, 1, 1}) == std::vector<int64_t>{-64, -48, -32, -16, 0});
    CHECK(coarse.SamplesPerSide(0) == 5);
    CHECK(coarse.CellSpacing(0) == 64.0);

    const TerrainRegionGrid uneven = Grid(layout, 1, 3);
    CHECK(uneven.LinesX({0, 1, 1}) == std::vector<int64_t>{-64, -43, -22, 0});
    CHECK(uneven.CellSpacing(0) == 88.0);
}

TEST_CASE("Region grids refuse a unit that does not divide the tile and empty sizes", "[terrain][region]")
{
    const TerrainLayout layout = ReliefLayout();
    CHECK_FALSE(TerrainRegionGrid::Create(layout, 16, 8, 3));
    CHECK_FALSE(TerrainRegionGrid::Create(layout, 16, 8, 0));
    CHECK_FALSE(TerrainRegionGrid::Create(layout, 0, 8, 1));
    CHECK_FALSE(TerrainRegionGrid::Create(layout, 16, 0, 1));
    CHECK_FALSE(TerrainRegionGrid::Create(layout, 16, kMaxTerrainRegionCells + 1, 1));
    CHECK(TerrainRegionGrid::Create(layout, 16, kMaxTerrainRegionCells, 64));
}

TEST_CASE("A release reads each Region by level and coordinate in one range and no tile payload",
          "[terrain][region][release]")
{
    const RegionRelease r(EdgeLayout(), 1, 8, {.chunkTiles = 2});
    auto [source, handle] = OpenInMemory(r.encoded);
    const TerrainIndex &index = handle.Index();
    REQUIRE(index.formatMinor == kTerrainFormatMinor);
    REQUIRE(index.regionGrid);
    CHECK(index.regionGrid->SameGrid(*r.regions.grid));
    REQUIRE(index.regions.size() == 14);
    CHECK(index.tiles.size() == r.tiles.size());
    CHECK(handle.VerifyAllPacks().code == Code::None);
    CHECK(index.heightsHash == r.encoded.heightsHash);
    std::vector<TerrainTileWrite> inputs;
    for (const TileHeights &tile : r.tiles)
        inputs.push_back({&tile});
    CHECK(TerrainPayloadsHash(handle.Layers(), inputs, {}, r.regions) == index.heightsHash);
    CHECK(TerrainHeightsHash(r.layout, r.tiles) != index.heightsHash);

    uint32_t packed = 0;
    for (const TerrainPackRecord &pack : index.packs)
        packed += pack.tileCount;
    CHECK(packed == index.tiles.size() + index.regions.size());

    for (uint64_t ordinal = 0; ordinal < index.regions.size(); ++ordinal)
    {
        const TerrainRegionCoord region = index.regionGrid->AtOrdinal(ordinal);
        INFO("Region L" << region.level << " (" << region.x << ", " << region.z << ")");
        const TerrainTileRecord *record = index.FindRegion(region);
        REQUIRE(record == &index.regions[ordinal]);
        CHECK(index.Find({region.x, region.z}, 0, TerrainPayloadKind::Region, region.level) == record);
        CHECK(index.Find({region.x, region.z}, 1, TerrainPayloadKind::Region, region.level) == nullptr);
        CHECK(index.packs[record->pack].chunk ==
              TerrainChunkOf(index.regionGrid->MapTiles(region).min, index.chunkTiles));
        CHECK(record->validExtent == index.regionGrid->ValidExtent(region));

        source->ResetStats();
        source->ClearRequests();
        const TerrainResult<TerrainRegionMesh> mesh = handle.ReadRegion(region);
        INFO(mesh.error.message);
        REQUIRE(mesh);
        REQUIRE(source->Requests().size() == 1);
        CHECK(source->Requests()[0].file == TerrainPackFileName(index.packs[record->pack].chunk));
        CHECK(source->Requests()[0].offset == record->offset);
        CHECK(source->Requests()[0].length == record->length);
        CHECK(mesh.value->release == r.info);
        CHECK(SameMesh(*mesh.value, DecodeWritten(r, ordinal)));

        const TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(*record);
        REQUIRE(bytes);
        CHECK(*bytes.value == r.regions.payloads[ordinal].bytes);
    }
    CHECK(handle.ReadRegion({0, 3, 0}).error.code == Code::NotFound);
    CHECK(handle.ReadRegion({3, 0, 0}).error.code == Code::NotFound);
    for (const TileHeights &tile : r.tiles)
        CHECK(ReadOk(handle, tile.Tile()) == tile);
}

TEST_CASE("Region meshes cover their domains and bound their distance to the final surface",
          "[terrain][region][error]")
{
    for (const TerrainLayout &layout : {EdgeLayout(), ReliefLayout()})
        for (const uint32_t regionTiles : {1u, 2u, 16u})
        {
            const RegionRelease r(layout, regionTiles, 8);
            const TerrainRegionGrid &grid = *r.regions.grid;
            const TileSampleLookup lookup = LookupOf(r.tiles);
            std::mt19937 random(124);
            for (size_t ordinal = 0; ordinal < r.regions.payloads.size(); ++ordinal)
            {
                const TerrainRegionMesh mesh = DecodeWritten(r, ordinal);
                INFO("Region L" << mesh.region.level << " (" << mesh.region.x << ", " << mesh.region.z << ")");
                CHECK(TerrainRegionCoversDomain(grid, mesh));
                const TerrainLocalBounds extent = grid.ValidExtent(mesh.region);
                const glm::dvec3 origin = mesh.origin.Meters();
                std::uniform_real_distribution<double> ux(extent.minX, extent.maxX), uz(extent.minZ, extent.maxZ);
                double worst = 0.0;
                for (int probe = 0; probe < 300; ++probe)
                {
                    const double x = ux(random), z = uz(random);
                    const SurfaceSample final =
                        EvaluateSurface(layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}), lookup);
                    REQUIRE(final.IsOk());
                    const std::optional<double> coarse = EvaluateTerrainRegion(mesh, x - origin.x, z - origin.z);
                    REQUIRE(coarse);
                    worst = std::max(worst, std::abs(*coarse - final.height));
                }
                CHECK(worst <= mesh.maxDeviation);
                for (const glm::vec3 &p : mesh.positions)
                {
                    const double x = origin.x + p.x, z = origin.z + p.z;
                    if (x < extent.minX || x > extent.maxX || z < extent.minZ || z > extent.maxZ)
                        continue;
                    const SurfaceSample final =
                        EvaluateSurface(layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}), lookup);
                    CHECK(double(p.y) == final.height);
                }
            }
        }
}

TEST_CASE("A Region error bound over whole aligned cells is attained at a source node", "[terrain][region][error]")
{
    const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-512.0, -512.0, 0.0, 0.0});
    REQUIRE(layout);
    const RegionRelease r(*layout, 1, 8);
    const TerrainRegionGrid &grid = *r.regions.grid;
    for (size_t ordinal = 0; ordinal < r.regions.payloads.size(); ++ordinal)
    {
        const TerrainRegionMesh mesh = DecodeWritten(r, ordinal);
        const glm::dvec3 origin = mesh.origin.Meters();
        const SampleRange domain = grid.Domain(mesh.region);
        double nodes = 0.0;
        for (int64_t k = domain.min.z; k <= domain.max.z; ++k)
            for (int64_t i = domain.min.x; i <= domain.max.x; ++i)
            {
                const double x = layout->SamplePosition(i), z = layout->SamplePosition(k);
                const SurfaceSample final =
                    EvaluateSurface(*layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}), LookupOf(r.tiles));
                REQUIRE(final.IsOk());
                nodes = std::max(nodes, std::abs(*EvaluateTerrainRegion(mesh, x - origin.x, z - origin.z) -
                                                 final.height));
            }
        CHECK(nodes > 0.0);
        CHECK(nodes <= mesh.maxDeviation);
        CHECK(mesh.maxDeviation - nodes < 1e-6);
    }
}

TEST_CASE("Malformed Region tables are refused before any Region is read", "[terrain][region][integrity]")
{
    const RegionRelease r(EdgeLayout(), 1, 8, {.chunkTiles = 2});
    const TerrainIndex index = *ParseTerrainIndex(r.encoded.index).value;
    const size_t first = RegionRecordAt(index, 0);
    const size_t second = RegionRecordAt(index, 1);
    auto refused = [&](const std::function<void(std::vector<std::byte> &)> &patch, Code code, std::string_view text) {
        std::vector<std::byte> bytes = r.encoded.index;
        patch(bytes);
        ResealIndex(bytes);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(bytes);
        CHECK_FALSE(parsed);
        CHECK(parsed.error.code == code);
        CHECK(Mentions(parsed.error, text));
    };
    auto f64 = [](double v) { return std::bit_cast<uint64_t>(v); };

    SECTION("header")
    {
        refused([](auto &b) { StoreU32(b, 12, kTerrainLegacyIndexHeaderSize); }, Code::Malformed, "header size");
        refused([](auto &b) { StoreU32(b, 132, kTerrainTileRecordSize - 8); }, Code::Malformed, "Region record size");
        refused([](auto &b) { StoreU32(b, 136, 0); }, Code::Malformed, "without Regions");
        refused([](auto &b) { StoreU32(b, 148, 2); }, Code::Malformed, "Region levels");
        refused([](auto &b) { StoreU32(b, 128, 13); }, Code::Malformed, "Region");
        refused([](auto &b) { StoreU32(b, 144, 3); }, Code::Malformed, "Region grid");
        refused([](auto &b) { StoreU32(b, 140, 0); }, Code::Malformed, "Region grid");
        refused([](auto &b) { StoreU32(b, 128, 0xFFFFFFFFu); }, Code::Malformed, "do not fit");
    }
    SECTION("an older minor cannot drop the Region table")
    {
        refused([](auto &b) { StoreU32(b, 8, 1); }, Code::Malformed, "point at it");
    }
    SECTION("addressing")
    {
        refused([&](auto &b) { std::swap_ranges(b.begin() + first, b.begin() + second, b.begin() + second); },
                Code::Malformed, "every Region once");
        refused([&](auto &b) { StoreU32(b, second, 0); }, Code::Malformed, "every Region once");
        refused([&](auto &b) { StoreU32(b, first + 96, 1); }, Code::Malformed, "every Region once");
        refused([&](auto &b) { StoreU32(b, first + 92, uint32_t(TerrainPayloadKind::LodHeight)); }, Code::Malformed,
                "kind");
        refused([&](auto &b) { StoreU32(b, first + 88, 1); }, Code::Malformed, "layer");
        refused([&](auto &b) { StoreU32(b, first + 100, 1); }, Code::Malformed, "source");
        refused([&](auto &b) { StoreU32(b, first + 100, 2); }, Code::Malformed, "flag");
    }
    SECTION("bounds and metadata")
    {
        refused([&](auto &b) { StoreU64(b, first + 56, f64(-300.0)); }, Code::Malformed, "valid extent");
        refused([&](auto &b) { StoreU64(b, first + 80, f64(1.0)); }, Code::Malformed, "valid extent");
        refused([&](auto &b) { StoreU32(b, first + 12, 8); }, Code::Malformed, "nodes per side");
        refused([&](auto &b) { StoreU64(b, first + 40, f64(16.0)); }, Code::Malformed, "nodes per side");
        refused([&](auto &b) { StoreU32(b, first + 48, std::bit_cast<uint32_t>(1e9f)); }, Code::Malformed,
                "height range");
        refused([&](auto &b) { StoreU32(b, first + 104, 0); }, Code::Malformed, "error bound");
        refused([&](auto &b) { StoreU64(b, first + 112, f64(-1.0)); }, Code::Malformed, "error bound");
        refused([&](auto &b) { StoreU64(b, first + 112, f64(std::numeric_limits<double>::quiet_NaN())); },
                Code::Malformed, "error bound");
        refused([&](auto &b) { StoreU64(b, first + 120, 0x1234); }, Code::Malformed, "source checksum");
        refused([&](auto &b) { StoreU32(b, first + 136, index.regions[0].vertexCount + 1); }, Code::Malformed,
                "payload length");
        refused([&](auto &b) { StoreU32(b, first + 140, 4); }, Code::Malformed, "payload length");
    }
    SECTION("offsets")
    {
        const TerrainTileRecord &region = index.regions[0];
        refused([&](auto &b) { StoreU32(b, first + 8, uint32_t(index.packs.size())); }, Code::Malformed,
                "out of range");
        refused([&](auto &b) { StoreU32(b, first + 8, region.pack + 1); }, Code::Malformed, "first map tile");
        refused([&](auto &b) { StoreU64(b, first + 16, region.offset + 4); }, Code::Malformed, "unaligned");
        refused([&](auto &b) { StoreU64(b, first + 16, 16); }, Code::Malformed, "pack header");
        refused([&](auto &b) { StoreU64(b, first + 16, (index.packs[region.pack].fileSize + 15) / 16 * 16); },
                Code::Malformed, "ends past");
        refused([&](auto &b) { StoreU64(b, first + 16, std::numeric_limits<uint64_t>::max() - 15); },
                Code::Malformed, "ends past");
        const TerrainTileRecord *neighbour = nullptr;
        for (const TerrainTileRecord &t : index.tiles)
            if (t.pack == region.pack)
                neighbour = &t;
        REQUIRE(neighbour);
        refused([&](auto &b) { StoreU64(b, first + 16, neighbour->offset); }, Code::Malformed, "overlap");
    }
    SECTION("a Region in the payload table")
    {
        const size_t tile = TileRecordAt(uint32_t(index.packs.size()), 0);
        refused([&](auto &b) { StoreU32(b, tile + 92, uint32_t(TerrainPayloadKind::Region)); }, Code::Unsupported,
                "unsupported payload kind");
    }
}

TEST_CASE("Damaged Region payloads are refused when read", "[terrain][region][integrity]")
{
    const RegionRelease r(EdgeLayout(), 1, 8);
    const TerrainIndex index = *ParseTerrainIndex(r.encoded.index).value;
    const size_t ordinal = 4;
    const TerrainTileRecord &record = index.regions[ordinal];
    const TerrainRegionMesh good = DecodeWritten(r, ordinal);
    auto refused = [&](TerrainRegionMesh mesh, Code code, std::string_view text,
                       const std::function<void(std::vector<std::byte> &)> &patch = {}) {
        std::vector<std::byte> bytes = EncodeTerrainRegion(mesh);
        if (patch)
            patch(bytes);
        TerrainTileRecord forged = record;
        forged.vertexCount = uint32_t(mesh.positions.size());
        forged.indexCount = uint32_t(mesh.indices.size());
        forged.length = bytes.size();
        forged.checksum = Fnv1a64(bytes);
        forged.topologyHash = mesh.topologyHash;
        forged.maxDeviation = mesh.maxDeviation;
        const TerrainResult<TerrainRegionMesh> decoded = DecodeTerrainRegion(index, forged, bytes);
        CHECK_FALSE(decoded);
        CHECK(decoded.error.code == code);
        CHECK(Mentions(decoded.error, text));
    };

    SECTION("container")
    {
        refused(good, Code::Unsupported, "content version", [](auto &b) { StoreU32(b, 4, 2); });
        refused(good, Code::Unsupported, "magic", [](auto &b) { StoreU32(b, 0, 0); });
        refused(good, Code::Corrupt, "header", [](auto &b) { StoreU32(b, 12, 7); });
        refused(good, Code::Corrupt, "header", [](auto &b) { StoreU64(b, 24, std::bit_cast<uint64_t>(1.0)); });
        refused(good, Code::Corrupt, "header", [](auto &b) { StoreU32(b, 56, 5); });
        refused(good, Code::Corrupt, "header", [](auto &b) { StoreU64(b, 96, 1); });
        std::vector<std::byte> bytes = r.regions.payloads[ordinal].bytes;
        bytes[bytes.size() / 2] ^= std::byte{0x20};
        const TerrainResult<TerrainRegionMesh> flipped = DecodeTerrainRegion(index, record, bytes);
        CHECK(flipped.error.code == Code::Corrupt);
        CHECK(Mentions(flipped.error, "checksum"));
        bytes.pop_back();
        CHECK(DecodeTerrainRegion(index, record, bytes).error.code == Code::Corrupt);
        TerrainTileRecord lod = record;
        lod.kind = TerrainPayloadKind::LodHeight;
        CHECK(DecodeTerrainRegion(index, lod, r.regions.payloads[ordinal].bytes).error.code == Code::InvalidInput);
    }
    SECTION("geometry")
    {
        TerrainRegionMesh outside = good;
        outside.positions[0].x -= 1.0f;
        outside.topologyHash = TerrainTopologyHash(outside.positions, outside.indices);
        refused(outside, Code::Corrupt, "outside the Region's domain");

        TerrainRegionMesh hole = good;
        hole.indices.resize(hole.indices.size() - 3);
        hole.topologyHash = TerrainTopologyHash(hole.positions, hole.indices);
        refused(hole, Code::Corrupt, "do not cover");

        TerrainRegionMesh flipped = good;
        std::swap(flipped.indices[1], flipped.indices[2]);
        flipped.topologyHash = TerrainTopologyHash(flipped.positions, flipped.indices);
        refused(flipped, Code::Corrupt, "wound");

        TerrainRegionMesh past = good;
        past.indices[0] = uint32_t(past.positions.size());
        refused(past, Code::Corrupt, "past the vertices");

        TerrainRegionMesh normal = good;
        normal.normals[0] = {0.0f, -1.0f, 0.0f};
        refused(normal, Code::Corrupt, "normal");
        normal.normals[0] = {0.0f, 2.0f, 0.0f};
        refused(normal, Code::Corrupt, "normal");

        TerrainRegionMesh higher = good;
        higher.positions[0].y += 100.0f;
        refused(higher, Code::Corrupt, "heights span");

        TerrainRegionMesh topology = good;
        topology.topologyHash ^= 1;
        refused(topology, Code::Corrupt, "topology");
    }
    SECTION("in the pack")
    {
        TerrainEncodedRelease damaged = r.encoded;
        TerrainEncodedPack &pack = damaged.packs[record.pack];
        pack.bytes[size_t(record.offset + record.length / 2)] ^= std::byte{0x01};
        auto [source, handle] = OpenInMemory(damaged);
        const TerrainRegionCoord region = index.regionGrid->AtOrdinal(ordinal);
        const TerrainResult<TerrainRegionMesh> read = handle.ReadRegion(region);
        CHECK(read.error.code == Code::Corrupt);
        CHECK(handle.VerifyAllPacks().code == Code::Corrupt);
        for (const TileHeights &tile : r.tiles)
            CHECK(ReadOk(handle, tile.Tile()) == tile);
    }
}

TEST_CASE("The writer refuses incomplete, repeated or foreign Region sets", "[terrain][region][pack]")
{
    const TerrainLayout layout = EdgeLayout();
    const std::vector<TileHeights> tiles = BuildTiles(layout);
    const TerrainRegionWrites regions = BuildTestRegions(layout, tiles, 1, 8);
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/regions.terrain.json"), 0x124);
    std::vector<TerrainTileWrite> inputs;
    for (const TileHeights &tile : tiles)
        inputs.push_back({&tile});
    auto refused = [&](const TerrainRegionWrites &w, std::string_view text) {
        const TerrainResult<TerrainEncodedRelease> encoded =
            EncodeTerrainRelease(info, std::span(&layout, 1), inputs, {}, w);
        CHECK_FALSE(encoded);
        CHECK(encoded.error.code == Code::InvalidInput);
        CHECK(Mentions(encoded.error, text));
    };

    TerrainRegionWrites missing = regions;
    missing.payloads.pop_back();
    refused(missing, "Region payloads for a grid");

    TerrainRegionWrites repeated = regions;
    repeated.payloads.back() = repeated.payloads.front();
    refused(repeated, "every Region once");

    TerrainRegionWrites foreign = regions;
    foreign.payloads[0].record.tile = {7, 7};
    refused(foreign, "names no Region");

    TerrainRegionWrites gridless = regions;
    gridless.grid.reset();
    refused(gridless, "need a Region grid");

    TerrainRegionWrites other = regions;
    other.grid = TerrainRegionGrid::Create(ReliefLayout(), 1, 8, 1);
    refused(other, "not over the reference layer");

    std::vector<TerrainPayloadWrite> reversed(regions.payloads.rbegin(), regions.payloads.rend());
    TerrainRegionWrites shuffled{regions.grid, reversed};
    const TerrainResult<TerrainEncodedRelease> any =
        EncodeTerrainRelease(info, std::span(&layout, 1), inputs, {}, shuffled);
    REQUIRE(any);
    CHECK(any.value->index == EncodeWithRegions(info, layout, tiles, regions).index);
}
