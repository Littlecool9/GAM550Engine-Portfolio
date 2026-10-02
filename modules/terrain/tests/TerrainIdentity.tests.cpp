/**
 * @file TerrainIdentity.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainIdentity and terrain/TileKey.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainIdentity.h>
#include <terrain/TileAddressing.h>
#include <terrain/TileKey.h>

#include <assets/AssetPath.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <unordered_set>

using namespace mir;

// Source, format and release identities never stand in for one another or for raw integers.
static_assert(!std::is_convertible_v<uint64_t, TerrainSourceFingerprint>);
static_assert(!std::is_convertible_v<uint32_t, TerrainFormatVersion>);
static_assert(!std::is_convertible_v<uint64_t, TerrainReleaseId>);
static_assert(!std::is_constructible_v<TerrainReleaseId, TerrainSourceFingerprint>);
static_assert(!std::is_constructible_v<TerrainSourceFingerprint, TerrainReleaseId>);
static_assert(!std::is_constructible_v<TerrainReleaseId, TerrainFormatVersion>);
static_assert(!std::is_constructible_v<TerrainReleaseId, AssetId>);
static_assert(!std::is_constructible_v<AssetId, TerrainReleaseId>);

namespace
{
    AssetId TerrainAsset(std::string_view relative)
    {
        const AssetPathResult logical = MakeLogicalAssetPath("games/HelloWorld/assets", relative);
        REQUIRE(logical.Ok());
        return MakeAssetId(logical.path);
    }
}

TEST_CASE("Release info tells source, format and release apart", "[terrain][identity]")
{
    const AssetId asset = TerrainAsset("terrain/island.terrain");
    const TerrainReleaseInfo first{asset, TerrainSourceFingerprint(0x1111), TerrainFormatVersion(1),
                                   TerrainReleaseId(0xA)};

    // Rebaking the same source is still a new release.
    TerrainReleaseInfo rebaked = first;
    rebaked.release = TerrainReleaseId(0xB);
    CHECK(rebaked != first);
    CHECK(rebaked.source == first.source);

    TerrainReleaseInfo newFormat = first;
    newFormat.format = TerrainFormatVersion(2);
    CHECK(newFormat != first);
    CHECK(newFormat.release == first.release);

    TerrainReleaseInfo editedSource = first;
    editedSource.source = TerrainSourceFingerprint(0x2222);
    CHECK(editedSource != first);

    CHECK(TerrainReleaseId().IsNull());
    CHECK(TerrainSourceFingerprint().IsNull());
    CHECK(TerrainFormatVersion().IsNull());
}

TEST_CASE("A terrain's AssetId follows its normalized logical path", "[terrain][identity]")
{
    const AssetId asset = TerrainAsset("terrain/island.terrain");
    CHECK(TerrainAsset("terrain\\island.terrain") == asset);
    CHECK(TerrainAsset("./terrain//island.terrain") == asset);

    // Case is part of the identity, and a move is a new identity: references
    // to the old path must be updated explicitly.
    CHECK(TerrainAsset("Terrain/Island.terrain") != asset);
    CHECK(TerrainAsset("terrain/archipelago/island.terrain") != asset);

    CHECK(MakeLogicalAssetPath("games/HelloWorld/assets", "../terrain/island.terrain").error ==
          AssetPathError::EscapesRoot);
}

TEST_CASE("Placements of one asset share tile keys but not world positions", "[terrain][identity]")
{
    const auto layout = TerrainLayout::Create(1.0, 256, {-1024.0, -1024.0, 1024.0, 1024.0});
    REQUIRE(layout);
    const AssetId asset = TerrainAsset("terrain/island.terrain");
    const TerrainPlacement west{asset, *WorldPosition::FromMeters({-50000.0, 0.0, 0.0})};
    const TerrainPlacement east{asset, *WorldPosition::FromMeters({50000.0, 10.0, 0.0})};

    const TileAddress fromWest = AddressWorld(*layout, west, *WorldPosition::FromMeters({-49700.5, 0.0, 12.25}));
    const TileAddress fromEast = AddressWorld(*layout, east, *WorldPosition::FromMeters({50299.5, 0.0, 12.25}));
    REQUIRE(fromWest.IsInside());
    REQUIRE(fromEast.IsInside());
    CHECK(fromWest.tile == fromEast.tile);
    CHECK(fromWest.withinTile == fromEast.withinTile);

    const TileKey westKey{west.asset, fromWest.tile};
    const TileKey eastKey{east.asset, fromEast.tile};
    CHECK(westKey == eastKey);

    const std::unordered_set<TileKey> keys{westKey, eastKey, TileKey{asset, {-1, 0}},
                                           TileKey{TerrainAsset("terrain/other.terrain"), fromWest.tile}};
    CHECK(keys.size() == 3);
}
