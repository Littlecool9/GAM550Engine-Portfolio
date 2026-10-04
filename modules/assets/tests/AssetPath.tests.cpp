/**
 * @file AssetPath.tests.cpp
 * @author Jackey Cen
 * @brief Tests for assets/AssetPath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <assets/AssetPath.h>

#include <assets/AssetId.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace
{
    mir::AssetId IdOf(std::string_view root, std::string_view relative)
    {
        const mir::AssetPathResult logical = mir::MakeLogicalAssetPath(root, relative);
        REQUIRE(logical.Ok());
        return mir::MakeAssetId(logical.path);
    }
}

TEST_CASE("AssetPath folds separator and dot aliases into one spelling", "[assets][path]")
{
    const char *aliases[]{
        "terrain/island.terrain",
        "terrain\\island.terrain",
        "terrain//island.terrain",
        "./terrain/./island.terrain",
        "terrain/tmp/../island.terrain",
        "terrain/island.terrain/",
    };
    for (const char *alias : aliases)
    {
        INFO(alias);
        const mir::AssetPathResult result = mir::NormalizeAssetPath(alias);
        REQUIRE(result.Ok());
        CHECK(result.path == "terrain/island.terrain");
    }
}

TEST_CASE("AssetPath keeps case and makes it part of the id", "[assets][path]")
{
    const mir::AssetPathResult upper = mir::NormalizeAssetPath("Terrain\\Island.terrain");
    REQUIRE(upper.Ok());
    CHECK(upper.path == "Terrain/Island.terrain");

    const std::string_view root = "games/HelloWorld/assets";
    CHECK(IdOf(root, "Terrain/Island.terrain") != IdOf(root, "terrain/island.terrain"));
    CHECK(IdOf(root, "terrain\\island.terrain") == IdOf(root, "terrain/island.terrain"));
}

TEST_CASE("AssetPath rejects escaping, absolute, empty and invalid paths", "[assets][path]")
{
    using mir::AssetPathError;
    CHECK(mir::NormalizeAssetPath("..").error == AssetPathError::EscapesRoot);
    CHECK(mir::NormalizeAssetPath("terrain/../../x").error == AssetPathError::EscapesRoot);
    CHECK(mir::NormalizeAssetPath("/terrain/a").error == AssetPathError::Absolute);
    CHECK(mir::NormalizeAssetPath("\\\\server\\share\\a").error == AssetPathError::Absolute);
    CHECK(mir::NormalizeAssetPath("C:/terrain/a").error == AssetPathError::Absolute);
    CHECK(mir::NormalizeAssetPath("c:terrain").error == AssetPathError::Absolute);
    CHECK(mir::NormalizeAssetPath("terrain/a:stream").error == AssetPathError::InvalidCharacter);
    CHECK(mir::NormalizeAssetPath(std::string_view("a\0b", 3)).error == AssetPathError::InvalidCharacter);
    CHECK(mir::NormalizeAssetPath("").error == AssetPathError::Empty);
    CHECK(mir::NormalizeAssetPath("./").error == AssetPathError::Empty);
    CHECK(mir::NormalizeAssetPath("a/..").error == AssetPathError::Empty);

    for (AssetPathError error : {AssetPathError::Empty, AssetPathError::Absolute, AssetPathError::InvalidCharacter,
                                 AssetPathError::EscapesRoot})
        CHECK_FALSE(mir::Describe(error).empty());
}

TEST_CASE("AssetPath does not let a relative path climb out of its root", "[assets][path]")
{
    // Lexically "games/HelloWorld/other/x" is still a fine path, but not under this root.
    const mir::AssetPathResult sibling = mir::MakeLogicalAssetPath("games/HelloWorld/assets", "../other/x");
    CHECK(sibling.error == mir::AssetPathError::EscapesRoot);

    const mir::AssetPathResult badRoot = mir::MakeLogicalAssetPath("../outside", "x");
    CHECK(badRoot.error == mir::AssetPathError::EscapesRoot);

    const mir::AssetPathResult inside = mir::MakeLogicalAssetPath("games/HelloWorld/assets", "meshes/../scripts/a.lua");
    REQUIRE(inside.Ok());
    CHECK(inside.path == "games/HelloWorld/assets/scripts/a.lua");
}

TEST_CASE("AssetPath leaves ids of already canonical paths unchanged", "[assets][path]")
{
    // The spellings the engine and AssetCooker used before AssetPath existed.
    struct Case
    {
        const char *root;
        const char *relative;
    };
    const Case cases[]{
        {"games/HelloWorld/assets", "meshes/cube.mesh.json"},
        {"games/HelloWorld/assets", "scripts/rotate.lua"},
        {"games/HelloWorld/assets", "models/Fox/Fox.gltf"},
        {"games/EmptyScene/assets", "scenes/a b/c.json"},
        {"assets/", "shaders/default.slang"},
    };
    for (const Case &c : cases)
    {
        INFO(c.root << " + " << c.relative);
        const std::string before =
            (std::filesystem::path(c.root) / c.relative).lexically_normal().generic_string();
        CHECK(mir::MakeAssetId(before) == IdOf(c.root, c.relative));
    }
}
