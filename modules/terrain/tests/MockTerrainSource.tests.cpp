/**
 * @file MockTerrainSource.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/MockTerrainSource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/MockTerrainSource.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <utility>

using namespace mir;

namespace
{
    // Exact in double at every integer sample, so equality checks are fair.
    double Plane(double x, double z)
    {
        return 0.25 * x - 0.5 * z + 100.0;
    }

    const AssetId kAsset = MakeAssetId("games/Test/assets/terrain/island.terrain");
    const TerrainReleaseInfo kRelease{kAsset, TerrainSourceFingerprint(0x5eed), TerrainFormatVersion(1),
                                      TerrainReleaseId(42)};

    WorldPosition World(double x, double y, double z)
    {
        return *WorldPosition::FromMeters({x, y, z});
    }

    MockTerrainSource MakeSource(const TerrainPlacement &placement, MockHeightFunction height = Plane)
    {
        const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -1000.5, 1000.5, 1000.5});
        REQUIRE(layout);
        auto source = MockTerrainSource::Create(*layout, kRelease, placement, std::move(height));
        REQUIRE(source);
        return *source;
    }
}

TEST_CASE("MockTerrainSource refuses a placement of another asset", "[terrain][mock]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1000.5, -1000.5, 1000.5, 1000.5});
    REQUIRE(layout);
    const TerrainPlacement other{MakeAssetId("games/Test/assets/terrain/other.terrain"), World(0.0, 0.0, 0.0)};
    CHECK_FALSE(MockTerrainSource::Create(*layout, kRelease, other, Plane));

    TerrainReleaseInfo unnamed = kRelease;
    unnamed.asset = kNullAssetId;
    CHECK_FALSE(MockTerrainSource::Create(*layout, unnamed, TerrainPlacement{kNullAssetId, {}}, Plane));

    CHECK_FALSE(MockTerrainSource::Create(*layout, kRelease, TerrainPlacement{kAsset, {}}, nullptr));
}

TEST_CASE("MockTerrainSource maps a far world position to key, release and world corners", "[terrain][mock][far]")
{
    const TerrainPlacement placement{kAsset, World(-150000.125, 350.5, 149999.875)};
    const MockTerrainSource source = MakeSource(placement);

    const MockLookup lookup = source.Lookup(World(-150000.625, 0.0, 150511.625));
    REQUIRE(lookup.address.IsInside());
    REQUIRE(lookup.key);
    CHECK(*lookup.key == TileKey{kAsset, {-1, 1}});
    REQUIRE(lookup.release);
    CHECK(*lookup.release == kRelease);

    // Corner = placement + (sample X, local height, sample Z), all binary-exact here.
    REQUIRE(lookup.cornerWorldPositions);
    const auto &corners = *lookup.cornerWorldPositions;
    CHECK(corners[0].Meters() == glm::dvec3(-150001.125, Plane(-1.0, 511.0) + 350.5, 150510.875));
    CHECK(corners[1].Meters() == glm::dvec3(-150000.125, Plane(0.0, 511.0) + 350.5, 150510.875));
    CHECK(corners[2].Meters() == glm::dvec3(-150001.125, Plane(-1.0, 512.0) + 350.5, 150511.875));
    CHECK(corners[3].Meters() == glm::dvec3(-150000.125, Plane(0.0, 512.0) + 350.5, 150511.875));
}

TEST_CASE("MockTerrainSource neighbours agree on shared edge and corner samples", "[terrain][mock]")
{
    const MockTerrainSource source = MakeSource(TerrainPlacement{kAsset, World(0.0, 0.0, 0.0)});

    // Cells on each side of the x = 256 tile edge share the samples on it.
    const MockLookup left = source.Lookup(World(255.5, 0.0, 10.5));
    const MockLookup right = source.Lookup(World(256.5, 0.0, 10.5));
    REQUIRE(left.cornerWorldPositions);
    REQUIRE(right.cornerWorldPositions);
    CHECK(left.key->coord == TileCoord{0, 0});
    CHECK(right.key->coord == TileCoord{1, 0});
    CHECK((*left.cornerWorldPositions)[1].Meters() == (*right.cornerWorldPositions)[0].Meters());
    CHECK((*left.cornerWorldPositions)[3].Meters() == (*right.cornerWorldPositions)[2].Meters());

    // The four tiles around the origin all reach sample (0, 0).
    const glm::dvec3 atOrigin(0.0, source.SampleHeightGlobal({0, 0})->localHeight, 0.0);
    CHECK((*source.Lookup(World(-0.5, 0.0, -0.5)).cornerWorldPositions)[3].Meters() == atOrigin);
    CHECK((*source.Lookup(World(0.5, 0.0, -0.5)).cornerWorldPositions)[2].Meters() == atOrigin);
    CHECK((*source.Lookup(World(-0.5, 0.0, 0.5)).cornerWorldPositions)[1].Meters() == atOrigin);
    CHECK((*source.Lookup(World(0.5, 0.0, 0.5)).cornerWorldPositions)[0].Meters() == atOrigin);
}

TEST_CASE("MockTerrainSource flags padding and refuses samples outside the map tiles", "[terrain][mock]")
{
    const MockTerrainSource source = MakeSource(TerrainPlacement{kAsset, World(0.0, 0.0, 0.0)});

    const auto inside = source.SampleHeightGlobal({1000, 0});
    REQUIRE(inside);
    CHECK_FALSE(inside->padding);
    CHECK(inside->localHeight == Plane(1000.0, 0.0));

    const auto padding = source.SampleHeightGlobal({1001, 0});
    REQUIRE(padding);
    CHECK(padding->padding);

    // Tile 3 ends at sample 1024; sample 1025 belongs to no map tile.
    CHECK(source.SampleHeightGlobal({1024, 0}));
    CHECK_FALSE(source.SampleHeightGlobal({1025, 0}));
    CHECK(source.SampleHeightGlobal({0, -1024}));
    CHECK_FALSE(source.SampleHeightGlobal({0, -1025}));
}

TEST_CASE("MockTerrainSource never reports a height it does not have", "[terrain][mock]")
{
    const MockTerrainSource source = MakeSource(TerrainPlacement{kAsset, World(0.0, 0.0, 0.0)});

    const MockLookup outside = source.Lookup(World(2000.0, 0.0, 0.0));
    CHECK(outside.address.status == TileAddressStatus::OutsideMap);
    CHECK_FALSE(outside.key);
    CHECK_FALSE(outside.release);
    CHECK_FALSE(outside.cornerWorldPositions);

    const MockTerrainSource far = MakeSource(TerrainPlacement{kAsset, World(-1.0e308, 0.0, 0.0)});
    const MockLookup invalid = far.Lookup(World(1.0e308, 0.0, 0.0));
    CHECK(invalid.address.status == TileAddressStatus::InvalidInput);
    CHECK_FALSE(invalid.cornerWorldPositions);

    // Local height + placement Y overflows: TryTerrainLocalToWorld refuses it, no inf corner.
    const MockTerrainSource high = MakeSource(TerrainPlacement{kAsset, World(0.0, 1.0e308, 0.0)},
                                              [](double, double) { return 1.0e308; });
    const MockLookup overflow = high.Lookup(World(0.5, 0.0, 0.5));
    REQUIRE(overflow.address.IsInside());
    CHECK(overflow.key);
    CHECK_FALSE(overflow.cornerWorldPositions);

    // A hole in the height function stays a hole, not a zero.
    const MockTerrainSource holed =
        MakeSource(TerrainPlacement{kAsset, World(0.0, 0.0, 0.0)},
                   [](double x, double z) { return x > 500.0 ? std::numeric_limits<double>::quiet_NaN() : Plane(x, z); });
    CHECK_FALSE(holed.SampleHeightGlobal({600, 0}));
    const MockLookup gap = holed.Lookup(World(600.5, 0.0, 0.5));
    REQUIRE(gap.address.IsInside());
    CHECK(gap.key);
    CHECK_FALSE(gap.cornerWorldPositions);
}
