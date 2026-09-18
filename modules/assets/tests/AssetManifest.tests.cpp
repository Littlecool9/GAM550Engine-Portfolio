/**
 * @file AssetManifest.tests.cpp
 * @author Rahul Nair
 * @brief Tests for assets/AssetManifest: build/parse round trip.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <assets/AssetManifest.h>

#include <assets/AssetBlob.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("AssetManifest build and parse round trip", "[assets][manifest]")
{
    const mir::AssetId first = mir::MakeAssetId("a/one.raw");
    const mir::AssetId second = mir::MakeAssetId("a/two.raw");

    const mir::AssetManifestSourceEntry entries[]{
        {{first, 1, 1}, "cooked/one.blob"},
        {{second, 2, 1}, "cooked/two.blob"},
    };

    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob(entries);
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(blob);
    REQUIRE(manifest.has_value());
    CHECK(manifest->Count() == 2);

    const mir::AssetManifestEntry *foundFirst = manifest->Find(first);
    REQUIRE(foundFirst != nullptr);
    CHECK(foundFirst->typeTag == 1);
    CHECK(manifest->PathOf(*foundFirst) == "cooked/one.blob");

    const mir::AssetManifestEntry *foundSecond = manifest->Find(second);
    REQUIRE(foundSecond != nullptr);
    CHECK(foundSecond->typeTag == 2);
    CHECK(manifest->PathOf(*foundSecond) == "cooked/two.blob");
}

TEST_CASE("AssetManifest Find returns nullptr for an unknown id", "[assets][manifest]")
{
    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob({});
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(blob);
    REQUIRE(manifest.has_value());
    CHECK(manifest->Find(mir::MakeAssetId("missing")) == nullptr);
}

TEST_CASE("AssetManifest Parse rejects a blob with the wrong type tag", "[assets][manifest]")
{
    const std::vector<std::byte> blob = mir::BuildAssetBlob(999, 1, {});
    CHECK_FALSE(mir::AssetManifest::Parse(blob).has_value());
}

TEST_CASE("AssetManifest rejects an unsupported content version", "[assets][manifest]")
{
    const mir::AssetManifestSourceEntry entries[]{
        {{mir::MakeAssetId("a/one.raw"), 1, 1}, "cooked/one.blob"},
    };

    // a well-formed manifest body, re-wrapped with a content version this build
    // does not know: the entry layout it describes may not be the current one
    const std::vector<std::byte> current = mir::BuildAssetManifestBlob(entries);
    const std::optional<mir::AssetBlobView> view = mir::ParseAssetBlob(current);
    REQUIRE(view.has_value());

    const std::vector<std::byte> future = mir::BuildAssetBlob(
        mir::kAssetManifestTypeTag, mir::kAssetManifestContentVersion + 1, view->content);

    CHECK_FALSE(mir::AssetManifest::Parse(future).has_value());
}

TEST_CASE("AssetManifest rejects duplicate asset ids", "[assets][manifest]")
{
    const mir::AssetId id = mir::MakeAssetId("a/one.raw");

    // what an id collision looks like on disk: silently keeping the first would
    // make the second asset permanently unreachable
    const mir::AssetManifestSourceEntry entries[]{
        {{id, 1, 1}, "cooked/one.blob"},
        {{id, 1, 1}, "cooked/two.blob"},
    };

    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob(entries);
    CHECK_FALSE(mir::AssetManifest::Parse(blob).has_value());
}
