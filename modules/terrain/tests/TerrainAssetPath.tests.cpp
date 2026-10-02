/**
 * @file TerrainAssetPath.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainAssetPath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace mir;

namespace
{
    // What LoadGameProject builds for games/G with assetsRoot "assets".
    constexpr std::string_view kIdRoot = "games/G/assets";
    constexpr std::string_view kLogical = "games/G/assets/terrain/hills.terrain.json";
}

TEST_CASE("Every spelling of one recipe path resolves to the cooker's id", "[terrain][assetpath]")
{
    const AssetId expected = MakeAssetId(kLogical);
    for (const auto &[root, path] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"games/G/assets", "terrain/hills.terrain.json"},
             {"games/G/assets", "terrain\\hills.terrain.json"},
             {"games\\G\\.\\assets\\", "./terrain//hills.terrain.json"},
             {"games/G/assets/", "terrain/old/../hills.terrain.json"},
             {"games/G/x/../assets", "terrain/./hills.terrain.json"},
         })
    {
        INFO(root << " + " << path);
        const TerrainAssetPath resolved = ResolveTerrainAssetPath(root, path);
        REQUIRE(resolved.Ok());
        CHECK(resolved.logicalPath == kLogical);
        CHECK(resolved.asset == expected);
        const AssetPathResult cooker = MakeLogicalAssetPath(root, path);
        REQUIRE(cooker.Ok());
        CHECK(MakeAssetId(cooker.path) == resolved.asset);
        CHECK(DescribeTerrainAssetPathError(resolved, root, path).empty());
    }

    const TerrainAssetPath once = ResolveTerrainAssetPath(kIdRoot, "terrain\\a\\..\\hills.terrain.json");
    REQUIRE(once.Ok());
    const std::string relative = once.logicalPath.substr(kIdRoot.size() + 1);
    const TerrainAssetPath twice = ResolveTerrainAssetPath(kIdRoot, relative);
    REQUIRE(twice.Ok());
    CHECK(twice.logicalPath == once.logicalPath);
    CHECK(twice.asset == once.asset);
}

TEST_CASE("A recipe path may not leave its id root or skip the recipe suffix", "[terrain][assetpath]")
{
    struct Bad
    {
        std::string_view root;
        std::string_view path;
        TerrainAssetPathError error;
        AssetPathError pathError;
    };
    using E = TerrainAssetPathError;
    using P = AssetPathError;
    for (const Bad &bad : {
             Bad{kIdRoot, "../hills.terrain.json", E::SourcePath, P::EscapesRoot},
             Bad{kIdRoot, "terrain/../../other/hills.terrain.json", E::SourcePath, P::EscapesRoot},
             Bad{kIdRoot, "terrain\\..\\..\\hills.terrain.json", E::SourcePath, P::EscapesRoot},
             Bad{kIdRoot, "/terrain/hills.terrain.json", E::SourcePath, P::Absolute},
             Bad{kIdRoot, "C:/terrain/hills.terrain.json", E::SourcePath, P::Absolute},
             Bad{kIdRoot, "", E::SourcePath, P::Empty},
             Bad{"../games/G/assets", "terrain/hills.terrain.json", E::IdRoot, P::EscapesRoot},
             Bad{"", "terrain/hills.terrain.json", E::IdRoot, P::Empty},
             Bad{kIdRoot, "terrain/hills.json", E::NotARecipe, P::None},
             Bad{kIdRoot, "terrain/hills.terrain.json.bak", E::NotARecipe, P::None},
             Bad{kIdRoot, "terrain/.terrain.json", E::NotARecipe, P::None},
             Bad{kIdRoot, "terrain/Hills.Terrain.JSON", E::NotARecipe, P::None},
         })
    {
        INFO(bad.root << " + " << bad.path);
        const TerrainAssetPath resolved = ResolveTerrainAssetPath(bad.root, bad.path);
        CHECK_FALSE(resolved.Ok());
        CHECK(resolved.error == bad.error);
        CHECK(resolved.pathError == bad.pathError);
        CHECK(resolved.asset.IsNull());
        CHECK_FALSE(DescribeTerrainAssetPathError(resolved, bad.root, bad.path).empty());
    }
    const TerrainAssetPath escape = ResolveTerrainAssetPath(kIdRoot, "../hills.terrain.json");
    CHECK(DescribeTerrainAssetPathError(escape, kIdRoot, "../hills.terrain.json") ==
          "recipe path \"../hills.terrain.json\": path climbs out of the asset root with '..'");
}

TEST_CASE("Case is kept and hashed: two spellings are two terrain assets", "[terrain][assetpath]")
{
    const TerrainAssetPath lower = ResolveTerrainAssetPath(kIdRoot, "terrain/hills.terrain.json");
    const TerrainAssetPath upper = ResolveTerrainAssetPath(kIdRoot, "terrain/Hills.terrain.json");
    const TerrainAssetPath root = ResolveTerrainAssetPath("games/G/Assets", "terrain/hills.terrain.json");
    REQUIRE(lower.Ok());
    REQUIRE(upper.Ok());
    REQUIRE(root.Ok());
    CHECK(upper.logicalPath == "games/G/assets/terrain/Hills.terrain.json");
    CHECK(root.logicalPath == "games/G/Assets/terrain/hills.terrain.json");
    CHECK(lower.asset != upper.asset);
    CHECK(lower.asset != root.asset);
    CHECK(upper.asset != root.asset);

    const std::filesystem::path releases = TerrainReleasesRoot("games/G");
    CHECK(TerrainCurrentPointerPath(releases, lower.asset) != TerrainCurrentPointerPath(releases, upper.asset));
}

TEST_CASE("A game's releases live beside its assets root", "[terrain][assetpath]")
{
    const std::filesystem::path root = TerrainReleasesRoot(std::filesystem::path("D:/src") / "games" / "G");
    CHECK(root.generic_string() == "D:/src/games/G/terrain/releases");
    const TerrainAssetPath hills = ResolveTerrainAssetPath(kIdRoot, "terrain/hills.terrain.json");
    REQUIRE(hills.Ok());
    CHECK(TerrainReleaseDir(root, hills.asset, TerrainReleaseId(0x00c0ffee12345678)).generic_string() ==
          "D:/src/games/G/terrain/releases/" + TerrainIdHex(hills.asset.value) + "/00c0ffee12345678");
    CHECK(root.generic_string().find(std::string(kIdRoot)) == std::string::npos);
}

TEST_CASE("Recipes and release files are excluded from raw cooking", "[terrain][assetpath]")
{
    for (const char *path : {
             "games/G/assets/terrain/hills.terrain.json",
             "terrain\\Hills.TERRAIN.json",
             "terrain/releases/5ada0b9d43f41166/current",
             "Terrain\\Releases\\5ada0b9d43f41166\\00c0ffee12345678\\release.tindex",
             "x/terrain/releases/a/b/notes.txt",
             "loose/chunk_0_0.tpack",
             "loose/release.TINDEX",
         })
    {
        INFO(path);
        CHECK(IsTerrainFileExcludedFromCook(path));
    }
    for (const char *path : {
             "games/G/assets/terrain/hills.json",
             "terrain/releases",
             "terrain/releases.txt",
             "terrain/notes/releases/x.txt",
             "scripts/terrain.lua",
             "textures/terrain.png",
         })
    {
        INFO(path);
        CHECK_FALSE(IsTerrainFileExcludedFromCook(path));
    }
}
