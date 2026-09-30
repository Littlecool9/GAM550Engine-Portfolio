/**
 * @file TileAddressing.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TileAddressing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TileAddressing.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace mir;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();

    TerrainLocalPosition Local(double x, double z)
    {
        return *TerrainLocalPosition::FromMeters({x, 0.0, z});
    }

    WorldPosition World(double x, double y, double z)
    {
        return *WorldPosition::FromMeters({x, y, z});
    }

    TileAddress AddressX(const TerrainLayout &layout, double x)
    {
        return AddressLocal(layout, Local(x, 0.5));
    }
}

TEST_CASE("Tile addressing follows floor on both sides of the origin", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-10000.0, -10000.0, 10000.0, 10000.0});
    REQUIRE(layout);

    const TileAddress negative = AddressLocal(*layout, Local(-0.5, 0.25));
    REQUIRE(negative.IsInside());
    CHECK(negative.tile == TileCoord{-1, 0});
    CHECK(negative.withinTile == glm::dvec2(255.5, 0.25));
    CHECK(negative.cell.origin == GlobalSampleIndex{-1, 0});
    CHECK(negative.cell.fraction == glm::dvec2(0.5, 0.25));

    const TileAddress deep = AddressLocal(*layout, Local(-5000.75, 4999.5));
    REQUIRE(deep.IsInside());
    CHECK(deep.tile == TileCoord{-20, 19});
    CHECK(deep.withinTile == glm::dvec2(119.25, 135.5));
    CHECK(deep.cell.origin == GlobalSampleIndex{-5001, 4999});
}

TEST_CASE("A point on a tile edge belongs to the larger tile, its neighbours do not", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-10000.0, -10000.0, 10000.0, 10000.0});
    REQUIRE(layout);

    for (int32_t tile : {-2, -1, 0, 1, 2})
    {
        const double edge = layout->SamplePosition(layout->TileFirstSample(tile));
        INFO("edge of tile " << tile << " at " << edge);

        const TileAddress on = AddressX(*layout, edge);
        CHECK(on.tile.x == tile);
        CHECK(on.withinTile.x == 0.0);
        CHECK(on.cell.origin.x == layout->TileFirstSample(tile));
        CHECK(on.cell.fraction.x == 0.0);

        // The indices are exact; the offsets are rounded and below the 0
        // edge (-4.9e-324) already read 256 and 1.
        const TileAddress below = AddressX(*layout, std::nextafter(edge, -kInf));
        CHECK(below.tile.x == tile - 1);
        CHECK(below.withinTile.x <= 256.0);
        CHECK(below.cell.origin.x == layout->TileFirstSample(tile) - 1);
        CHECK(below.cell.fraction.x <= 1.0);

        const TileAddress above = AddressX(*layout, std::nextafter(edge, kInf));
        CHECK(above.tile.x == tile);
        CHECK(above.cell.origin.x == layout->TileFirstSample(tile));
    }
}

TEST_CASE("A four-tile corner belongs to the tile with both larger indices", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-10000.0, -10000.0, 10000.0, 10000.0});
    REQUIRE(layout);

    CHECK(AddressLocal(*layout, Local(0.0, 0.0)).tile == TileCoord{0, 0});
    CHECK(AddressLocal(*layout, Local(256.0, 256.0)).tile == TileCoord{1, 1});
    CHECK(AddressLocal(*layout, Local(-256.0, -256.0)).tile == TileCoord{-1, -1});
    CHECK(AddressLocal(*layout, Local(256.0, -256.0)).tile == TileCoord{1, -1});

    const TileAddress corner = AddressLocal(*layout, Local(-256.0, 512.0));
    CHECK(corner.tile == TileCoord{-1, 2});
    CHECK(corner.withinTile == glm::dvec2(0.0, 0.0));
}

TEST_CASE("The closed max edge uses the last cell with fraction one", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {0.0, 0.0, 256.0, 256.0});
    REQUIRE(layout);

    const TileAddress corner = AddressLocal(*layout, Local(256.0, 256.0));
    REQUIRE(corner.IsInside());
    CHECK(corner.tile == TileCoord{0, 0});
    CHECK(corner.withinTile == glm::dvec2(256.0, 256.0));
    CHECK(corner.cell.origin == GlobalSampleIndex{255, 255});
    CHECK(corner.cell.fraction == glm::dvec2(1.0, 1.0));

    const TileAddress xEdge = AddressLocal(*layout, Local(256.0, 10.25));
    REQUIRE(xEdge.IsInside());
    CHECK(xEdge.tile == TileCoord{0, 0});
    CHECK(xEdge.cell.origin == GlobalSampleIndex{255, 10});
    CHECK(xEdge.cell.fraction == glm::dvec2(1.0, 0.25));

    const TileAddress zEdge = AddressLocal(*layout, Local(3.5, 256.0));
    REQUIRE(zEdge.IsInside());
    CHECK(zEdge.cell.origin == GlobalSampleIndex{3, 255});
    CHECK(zEdge.cell.fraction == glm::dvec2(0.5, 1.0));

    // The min edge is closed too, and starts its own cell.
    const TileAddress origin = AddressLocal(*layout, Local(0.0, 0.0));
    REQUIRE(origin.IsInside());
    CHECK(origin.cell.origin == GlobalSampleIndex{0, 0});

    CHECK(AddressLocal(*layout, Local(std::nextafter(256.0, kInf), 1.0)).status == TileAddressStatus::OutsideMap);
    CHECK(AddressLocal(*layout, Local(1.0, std::nextafter(256.0, kInf))).status == TileAddressStatus::OutsideMap);
    CHECK(AddressLocal(*layout, Local(std::nextafter(0.0, -kInf), 1.0)).status == TileAddressStatus::OutsideMap);
    CHECK(AddressLocal(*layout, Local(1.0, -0.001)).status == TileAddressStatus::OutsideMap);
}

TEST_CASE("A non-aligned max edge uses the cell containing it and a padding sample", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -1000.5, 1000.5, 1000.5});
    REQUIRE(layout);

    const TileAddress high = AddressLocal(*layout, Local(1000.5, 1000.5));
    REQUIRE(high.IsInside());
    CHECK(high.tile == TileCoord{3, 3});
    CHECK(high.withinTile == glm::dvec2(232.5, 232.5));
    CHECK(high.cell.origin == GlobalSampleIndex{1000, 1000});
    CHECK(high.cell.fraction == glm::dvec2(0.5, 0.5));
    CHECK(layout->IsPaddingSample({1001, 1000}));

    const TileAddress low = AddressLocal(*layout, Local(-1000.5, -1000.5));
    REQUIRE(low.IsInside());
    CHECK(low.tile == TileCoord{-4, -4});
    CHECK(low.cell.origin == GlobalSampleIndex{-1001, -1001});
    CHECK(low.cell.fraction == glm::dvec2(0.5, 0.5));
    CHECK(layout->IsPaddingSample(low.cell.origin));

    CHECK(AddressLocal(*layout, Local(std::nextafter(1000.5, kInf), 0.0)).status == TileAddressStatus::OutsideMap);
    CHECK(AddressLocal(*layout, Local(0.0, std::nextafter(-1000.5, -kInf))).status ==
          TileAddressStatus::OutsideMap);
}

TEST_CASE("Tile edges agree with SamplePosition for non-binary and non-power-of-two sizes", "[terrain][address]")
{
    // 0.1 m has no exact binary form; T = 100 m is not a power of two.
    const auto decimal = TerrainLayout::Create(0.1, 2560, {-1000.0, -1000.0, 1000.0, 1000.0});
    const auto hundred = TerrainLayout::CreateFromTileSize(100.0, 4.0, {-1000.0, -1000.0, 1000.0, 1000.0});
    REQUIRE(decimal);
    REQUIRE(hundred);

    for (const TerrainLayout *layout : {&*decimal, &*hundred})
    {
        for (int32_t tile : {-3, -1, 0, 1, 3})
        {
            const int64_t first = layout->TileFirstSample(tile);
            const double edge = layout->SamplePosition(first);
            INFO("spacing " << layout->SampleSpacing() << ", tile " << tile << ", edge " << edge);

            const TileAddress on = AddressX(*layout, edge);
            CHECK(on.tile.x == tile);
            CHECK(on.cell.origin.x == first);
            CHECK(on.withinTile.x == 0.0);

            const TileAddress below = AddressX(*layout, std::nextafter(edge, -kInf));
            CHECK(below.tile.x == tile - 1);
            CHECK(below.cell.origin.x == first - 1);

            const TileAddress above = AddressX(*layout, std::nextafter(edge, kInf));
            CHECK(above.tile.x == tile);
            CHECK(above.cell.origin.x == first);
        }

        for (int64_t sample : {-9999, -13, 7, 1234, 9999})
        {
            const double x = layout->SamplePosition(sample);
            if (x < -1000.0 || x > 1000.0)
                continue;
            INFO("sample " << sample << " at " << x);
            const TileAddress at = AddressX(*layout, x);
            CHECK(at.cell.origin.x == sample);
            CHECK(at.cell.fraction.x == 0.0);
        }
    }
}

TEST_CASE("A far placement addresses exactly and float world positions would not", "[terrain][address][far]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-2048.0, -2048.0, 2048.0, 2048.0});
    REQUIRE(layout);
    const TerrainPlacement placement{MakeAssetId("games/Test/assets/terrain/island.terrain"),
                                     World(-150000.125, 350.5, 149999.875)};

    // Binary-exact local offsets: the address is exact.
    const TileAddress exact = AddressWorld(*layout, placement, World(-150000.625, 362.5, 150511.625));
    REQUIRE(exact.IsInside());
    CHECK(exact.tile == TileCoord{-1, 1});
    CHECK(exact.withinTile == glm::dvec2(255.5, 255.75));
    CHECK(exact.cell.origin == GlobalSampleIndex{-1, 511});
    CHECK(exact.cell.fraction == glm::dvec2(0.5, 0.75));

    // Local (-0.001, 255.999): just left of tile 0 and just below tile 1.
    const glm::dvec3 world(-150000.126, 357.5, 150255.874);
    const TileAddress small = AddressWorld(*layout, placement, World(world.x, world.y, world.z));
    REQUIRE(small.IsInside());
    CHECK(small.tile == TileCoord{-1, 0});
    CHECK(small.withinTile.x == Catch::Approx(255.999).margin(1e-9));
    CHECK(small.withinTile.y == Catch::Approx(255.999).margin(1e-9));

    // Narrowing both positions to float first rounds the offsets to 0 and 256.
    const glm::dvec3 origin = placement.translation.Meters();
    const float floatX = static_cast<float>(world.x) - static_cast<float>(origin.x);
    const float floatZ = static_cast<float>(world.z) - static_cast<float>(origin.z);
    CHECK(floatX == 0.0f);
    CHECK(floatZ == 256.0f);
    CHECK(AddressLocal(*layout, Local(floatX, floatZ)).tile == TileCoord{0, 1});
}

TEST_CASE("A far placement on the negative Z side keeps negative tiles", "[terrain][address][far]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-2048.0, -2048.0, 2048.0, 2048.0});
    REQUIRE(layout);
    const TerrainPlacement placement{MakeAssetId("games/Test/assets/terrain/island.terrain"),
                                     World(150000.25, -20.0, -150000.75)};

    const TileAddress address = AddressWorld(*layout, placement, World(150512.5, 0.0, -150001.0));
    REQUIRE(address.IsInside());
    CHECK(address.tile == TileCoord{2, -1});
    CHECK(address.withinTile == glm::dvec2(0.25, 255.75));
    CHECK(address.cell.origin == GlobalSampleIndex{512, -1});
    CHECK(address.cell.fraction == glm::dvec2(0.25, 0.75));

    CHECK(AddressWorld(*layout, placement, World(150000.25 + 2048.5, 0.0, -150000.75)).status ==
          TileAddressStatus::OutsideMap);
}

TEST_CASE("An overflowing world-to-local subtraction is invalid input", "[terrain][address]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-2048.0, -2048.0, 2048.0, 2048.0});
    REQUIRE(layout);
    const TerrainPlacement placement{MakeAssetId("games/Test/assets/terrain/island.terrain"),
                                     World(-1.0e308, 0.0, 0.0)};

    const TileAddress address = AddressWorld(*layout, placement, World(1.0e308, 0.0, 0.0));
    CHECK(address.status == TileAddressStatus::InvalidInput);
    CHECK_FALSE(address.IsInside());
}
