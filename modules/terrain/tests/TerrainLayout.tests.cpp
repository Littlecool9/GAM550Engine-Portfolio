/**
 * @file TerrainLayout.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainLayout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace mir;

namespace
{
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();

    const TerrainLocalBounds kSquare{-1000.0, -1000.0, 1000.0, 1000.0};
}

TEST_CASE("TerrainLayout rejects invalid spacing, tile and bounds", "[terrain][layout]")
{
    CHECK_FALSE(TerrainLayout::Create(0.0, 256, kSquare));
    CHECK_FALSE(TerrainLayout::Create(-1.0, 256, kSquare));
    CHECK_FALSE(TerrainLayout::Create(kNaN, 256, kSquare));
    CHECK_FALSE(TerrainLayout::Create(kInf, 256, kSquare));
    CHECK_FALSE(TerrainLayout::Create(1.0, 0, kSquare));
    CHECK_FALSE(TerrainLayout::Create(1.0, -4, kSquare));

    CHECK_FALSE(TerrainLayout::Create(1.0, 256, {kNaN, 0.0, 10.0, 10.0}));
    CHECK_FALSE(TerrainLayout::Create(1.0, 256, {0.0, 0.0, kInf, 10.0}));
    CHECK_FALSE(TerrainLayout::Create(1.0, 256, {10.0, 0.0, 10.0, 10.0}));
    CHECK_FALSE(TerrainLayout::Create(1.0, 256, {0.0, 10.0, 10.0, -10.0}));

    CHECK(TerrainLayout::Create(1.0, 256, kSquare));
}

TEST_CASE("TerrainLayout from a tile size needs an exact interval count", "[terrain][layout]")
{
    const auto layout = TerrainLayout::CreateFromTileSize(256.0, 1.0, kSquare);
    REQUIRE(layout);
    CHECK(layout->IntervalsPerTile() == 256);
    CHECK(layout->TileSize() == 256.0);

    const auto hundred = TerrainLayout::CreateFromTileSize(100.0, 4.0, kSquare);
    REQUIRE(hundred);
    CHECK(hundred->IntervalsPerTile() == 25);

    // 3 * 0.1 is 0.30000000000000004 in double, so 0.3 is not rebuilt exactly.
    CHECK_FALSE(TerrainLayout::CreateFromTileSize(0.3, 0.1, kSquare));
    CHECK(TerrainLayout::Create(0.1, 3, kSquare));

    CHECK_FALSE(TerrainLayout::CreateFromTileSize(256.0, 3.0, kSquare));
    CHECK_FALSE(TerrainLayout::CreateFromTileSize(1.0, 4.0, kSquare));
    CHECK_FALSE(TerrainLayout::CreateFromTileSize(kNaN, 1.0, kSquare));
}

TEST_CASE("TerrainLayout refuses tile indices past int32", "[terrain][layout]")
{
    // 3e9 one-meter tiles: sample indices fit int64, tile indices do not fit int32.
    CHECK_FALSE(TerrainLayout::Create(1.0, 1, {-3.0e9, 0.0, 3.0e9, 1.0}));
    CHECK(TerrainLayout::Create(1.0, 256, {-3.0e9, 0.0, 3.0e9, 1.0}));
}

TEST_CASE("TerrainLayout refuses a spacing doubles cannot resolve across the map", "[terrain][layout]")
{
    // Near 151 km the double ulp is 2^-35 m; 2^16 of them is about 1.9 um.
    const TerrainLocalBounds farMap{-150000.0, -150000.0, 150000.0, 150000.0};
    CHECK_FALSE(TerrainLayout::Create(1.0e-6, 1'000'000'000, farMap));
    CHECK(TerrainLayout::Create(2.0e-6, 500'000'000, farMap));
}

TEST_CASE("TerrainLayout clips edge tiles to non-aligned bounds", "[terrain][layout]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -300.0, 1000.5, 512.0});
    REQUIRE(layout);

    const TileRange tiles = layout->Tiles();
    CHECK(tiles.min == TileCoord{-4, -2});
    // maxZ = 512 is a tile edge; the closed edge stays in tile 1 and adds no empty tile 2.
    CHECK(tiles.max == TileCoord{3, 1});
    CHECK_FALSE(layout->Contains({4, 0}));
    CHECK_FALSE(layout->Contains({3, 2}));
    CHECK_FALSE(layout->ValidExtent({3, 2}));
    CHECK_FALSE(layout->TileSamples({-5, 0}));

    const auto low = layout->ValidExtent({-4, -2});
    REQUIRE(low);
    CHECK(low->minX == -1000.5);
    CHECK(low->maxX == -768.0);
    CHECK(low->minZ == -300.0);
    CHECK(low->maxZ == -256.0);

    const auto high = layout->ValidExtent({3, 1});
    REQUIRE(high);
    CHECK(high->minX == 768.0);
    CHECK(high->maxX == 1000.5);
    CHECK(high->minZ == 256.0);
    CHECK(high->maxZ == 512.0);

    const auto interior = layout->ValidExtent({0, 0});
    REQUIRE(interior);
    CHECK(interior->minX == 0.0);
    CHECK(interior->minZ == 0.0);
    CHECK(interior->maxX == 256.0);
    CHECK(interior->maxZ == 256.0);
}

TEST_CASE("TerrainLayout neighbours share edge sample indices", "[terrain][layout]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -300.0, 1000.5, 512.0});
    REQUIRE(layout);

    const auto left = layout->TileSamples({-1, 0});
    const auto right = layout->TileSamples({0, 0});
    const auto up = layout->TileSamples({0, 1});
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(up);
    CHECK(left->min == GlobalSampleIndex{-256, 0});
    CHECK(left->max == GlobalSampleIndex{0, 256});
    CHECK(left->max.x == right->min.x);
    CHECK(right->max.z == up->min.z);
    CHECK(layout->SamplePosition(right->min.x) == 0.0);
    CHECK(layout->SamplePosition(right->max.x) == 256.0);
}

TEST_CASE("TerrainLayout marks samples outside the bounds as padding", "[terrain][layout]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -300.0, 1000.5, 512.0});
    REQUIRE(layout);

    CHECK(layout->IsPaddingSample({-1001, 0}));
    CHECK_FALSE(layout->IsPaddingSample({-1000, 0}));
    CHECK_FALSE(layout->IsPaddingSample({1000, 0}));
    CHECK(layout->IsPaddingSample({1001, 0}));
    CHECK(layout->IsPaddingSample({0, -301}));
    CHECK_FALSE(layout->IsPaddingSample({0, 512}));
}
