/**
 * @file TerrainAreaSamples.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainAreaSamples.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainTileMesh.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    constexpr float kPoison = 1.0e6f;

    TileSampleLookup LookupOf(const std::vector<TileHeights> &tiles)
    {
        return [&tiles](TileCoord coord)
        {
            for (const TileHeights &tile : tiles)
                if (tile.Tile() == coord)
                    return tile.Samples();
            return std::span<const float>{};
        };
    }

    // Poisons the upper-edge copies, which belong to the next tile, so a block reading them is wrong.
    std::vector<TileHeights> OwnedOnly(const TerrainLayout &layout)
    {
        std::vector<TileHeights> tiles = BuildTiles(layout);
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (TileHeights &tile : tiles)
            for (int32_t j = 0; j <= n; ++j)
            {
                if (tile.Tile().x < range.max.x)
                    tile.Set(n, j, kPoison);
                if (tile.Tile().z < range.max.z)
                    tile.Set(j, n, kPoison);
            }
        return tiles;
    }

    // The union of the tile meshes' sample ranges.
    SampleRange MeshedSamples(const TerrainLayout &layout, const TerrainReleaseInfo &info, const TileSampleLookup &lookup)
    {
        SampleRange all{{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()},
                        {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min()}};
        const TileRange range = layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                const TerrainMeshResult mesh = BuildTerrainTileMesh(layout, info, {x, z}, lookup);
                REQUIRE(mesh.IsOk());
                all.min = {std::min(all.min.x, mesh.mesh->samples.min.x), std::min(all.min.z, mesh.mesh->samples.min.z)};
                all.max = {std::max(all.max.x, mesh.mesh->samples.max.x), std::max(all.max.z, mesh.mesh->samples.max.z)};
            }
        return all;
    }
}

TEST_CASE("The collider block covers exactly the meshed cells, each sample from its owning tile", "[terrain][area]")
{
    const auto aligned = TerrainLayout::CreateFromTileSize(256.0, 4.0, {-256.0, -256.0, 256.0, 256.0});
    REQUIRE(aligned);
    struct Case
    {
        TerrainLayout layout;
        SampleRange expected;
    };
    const Case cases[]{{ReliefLayout(), {{-63, -64}, {64, 63}}},
                       {EdgeLayout(), {{-76, -3}, {26, 65}}},
                       {*aligned, {{-64, -64}, {64, 64}}}};

    for (const Case &c : cases)
    {
        const TerrainReleaseInfo info = Info(TerrainAsset("terrain/area.terrain.json"), 0xA2EA);
        const std::vector<TileHeights> tiles = OwnedOnly(c.layout);
        const TileSampleLookup lookup = LookupOf(tiles);

        const TerrainResult<TerrainAreaSamples> built = BuildTerrainAreaSamples(c.layout, info, lookup);
        INFO(built.error.message);
        REQUIRE(built);
        const TerrainAreaSamples &area = *built.value;
        CHECK(area.release == info);
        CHECK(area.surfaceVersion == kTerrainSurfaceVersion);
        CHECK(area.samples.min == c.expected.min);
        CHECK(area.samples.max == c.expected.max);
        const SampleRange meshed = MeshedSamples(c.layout, info, lookup);
        CHECK(area.samples.min == meshed.min);
        CHECK(area.samples.max == meshed.max);
        CHECK(area.countX == uint32_t(area.samples.max.x - area.samples.min.x + 1));
        CHECK(area.countZ == uint32_t(area.samples.max.z - area.samples.min.z + 1));
        CHECK(area.heights.size() == size_t(area.countX) * area.countZ);
        CHECK(area.spacing == c.layout.SampleSpacing());
        CHECK(area.origin.Meters() == glm::dvec3(c.layout.SamplePosition(area.samples.min.x), 0.0,
                                                 c.layout.SamplePosition(area.samples.min.z)));

        size_t wrong = 0;
        for (int64_t k = area.samples.min.z; k <= area.samples.max.z; ++k)
            for (int64_t i = area.samples.min.x; i <= area.samples.max.x; ++i)
                wrong += area.heights[size_t((k - area.samples.min.z) * area.countX + (i - area.samples.min.x))] !=
                         AnalyticHeight(i, k);
        CHECK(wrong == 0);
    }
}

TEST_CASE("The collider block refuses missing, malformed and non-finite samples it owns", "[terrain][area]")
{
    const TerrainLayout layout = ReliefLayout();
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/area.terrain.json"), 0xA2EB);
    std::vector<TileHeights> tiles = OwnedOnly(layout);

    CHECK(BuildTerrainAreaSamples(layout, info, nullptr).error.code == Code::InvalidInput);

    const TileSampleLookup all = LookupOf(tiles);
    const TerrainResult<TerrainAreaSamples> missing = BuildTerrainAreaSamples(
        layout, info, [&](TileCoord tile) { return tile == TileCoord{0, 0} ? std::span<const float>() : all(tile); });
    CHECK(missing.error.code == Code::NotFound);
    CHECK(Mentions(missing.error, "(0, 0)"));

    const TerrainResult<TerrainAreaSamples> shortBlock =
        BuildTerrainAreaSamples(layout, info, [&](TileCoord tile) { return all(tile).first(10); });
    CHECK(shortBlock.error.code == Code::Corrupt);

    const int32_t n = layout.IntervalsPerTile();
    tiles[0].Set(n, 5, std::numeric_limits<float>::quiet_NaN());
    CHECK(BuildTerrainAreaSamples(layout, info, LookupOf(tiles)));
    tiles[0].Set(3, 5, std::numeric_limits<float>::infinity());
    const TerrainResult<TerrainAreaSamples> nonFinite = BuildTerrainAreaSamples(layout, info, LookupOf(tiles));
    CHECK(nonFinite.error.code == Code::Corrupt);
    CHECK(Mentions(nonFinite.error, "not finite"));

    const auto huge = TerrainLayout::Create(1.0, 64, {0.0, 0.0, 40000.0, 64.0});
    REQUIRE(huge);
    bool read = false;
    const TerrainResult<TerrainAreaSamples> tooLarge = BuildTerrainAreaSamples(*huge, info, [&](TileCoord)
    {
        read = true;
        return std::span<const float>();
    });
    CHECK(tooLarge.error.code == Code::Unsupported);
    CHECK_FALSE(read);
}
