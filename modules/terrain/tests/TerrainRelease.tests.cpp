/**
 * @file TerrainRelease.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainRelease and terrain/TerrainByteSource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    TerrainLayout SmallLayout()
    {
        const auto layout = TerrainLayout::Create(2.0, 4, {-10.0, -6.0, 9.0, 7.0});
        REQUIRE(layout);
        return *layout;
    }

    // The committed fixture: 2 m samples, n = 4, tiles (-2..1, -1..0), two tiles per pack.
    struct Fixture
    {
        AssetId asset = TerrainAsset("terrain/fixture.terrain.json");
        TerrainReleaseInfo info = Info(asset, 0x00c0ffee12345678);
        TerrainLayout layout = SmallLayout();
        TerrainPackWriteOptions options{.chunkTiles = 2};
    };

    std::filesystem::path FixtureRoot()
    {
        return std::filesystem::path(TERRAIN_FIXTURE_DIR) / "releases";
    }
}

TEST_CASE("Release directories and the current pointer follow the naming contract", "[terrain][release]")
{
    const std::filesystem::path root = "games/G/terrain/releases";
    const AssetId asset{0x1234};
    const TerrainReleaseId release(0xabcdef);

    CHECK(TerrainIdHex(0x1234) == "0000000000001234");
    CHECK(TerrainReleaseDir(root, asset, release) == root / "0000000000001234" / "0000000000abcdef");
    CHECK(TerrainCurrentPointerPath(root, asset) == root / "0000000000001234" / "current");
    CHECK(EncodeTerrainCurrentPointer(release) == "0000000000abcdef");

    CHECK(ParseTerrainIdHex(TerrainIdHex(0xfedcba9876543210)) == 0xfedcba9876543210);
    CHECK(ParseTerrainIdHex("0000000000000000") == 0u);
    for (std::string_view bad : {"", "1234", "0000000000001234\n", "000000000000ABCD", "+000000000001234"})
        CHECK_FALSE(ParseTerrainIdHex(bad));
}

TEST_CASE("The current pointer holds exactly one release id", "[terrain][release]")
{
    const TerrainReleaseId id(0x00c0ffee12345678);
    const TerrainResult<TerrainReleaseId> exact = ParseTerrainCurrentPointer(EncodeTerrainCurrentPointer(id));
    REQUIRE(exact);
    CHECK(*exact.value == id);
    CHECK(*ParseTerrainCurrentPointer("00c0ffee12345678\n").value == id);
    CHECK(*ParseTerrainCurrentPointer("00c0ffee12345678\r\n").value == id);

    for (std::string_view bad : {"", "00c0ffee1234567", "00c0ffee123456789", "00C0FFEE12345678", "00c0ffee1234567g",
                                 " 00c0ffee12345678", "00c0ffee12345678\n\n", "0000000000000000"})
    {
        INFO("pointer '" << bad << "'");
        const TerrainResult<TerrainReleaseId> parsed = ParseTerrainCurrentPointer(bad);
        CHECK_FALSE(parsed.value.has_value());
        CHECK(parsed.error.code == Code::Malformed);
    }
}

TEST_CASE("A release on disk reads one tile by its byte range", "[terrain][release]")
{
    const std::filesystem::path root = MakeScratchDir("range");
    const Fixture f;
    const std::vector<TileHeights> tiles = BuildTiles(f.layout);
    const TerrainEncodedRelease encoded = Encode(f.info, f.layout, tiles, f.options);
    const std::filesystem::path dir = WriteRelease(root, f.info, encoded);

    TerrainReleaseHandle handle = OpenOk(OpenTerrainRelease(root, f.asset, f.info.release));
    CHECK(handle.Info() == f.info);
    CHECK(handle.Source().Stats().reads == 1);
    CHECK(handle.Source().Stats().bytes == encoded.index.size());

    handle.Source().ResetStats();
    const TileCoord coord{1, 0};
    CHECK(ReadOk(handle, coord) == tiles[7]);
    const TerrainTileRecord *record = handle.Index().Find(coord);
    REQUIRE(record);
    CHECK(handle.Source().Stats().reads == 1);
    CHECK(handle.Source().Stats().bytes == record->length);
    CHECK(record->length == 100);
    CHECK(*FileSize(dir / "chunk_0_0.tpack") == 260);

    CHECK(handle.VerifyAllPacks().code == Code::None);
    std::filesystem::remove_all(root);
}

TEST_CASE("Opening by id refuses files that belong to another asset or release", "[terrain][release]")
{
    const std::filesystem::path root = MakeScratchDir("identity");
    const Fixture f;
    const TerrainEncodedRelease encoded = Encode(f.info, f.layout, BuildTiles(f.layout), f.options);

    // Release 0x...78's files copied under another release id.
    TerrainReleaseInfo renamed = f.info;
    renamed.release = TerrainReleaseId(0x99);
    WriteRelease(root, renamed, encoded);
    const TerrainResult<TerrainReleaseHandle> wrongRelease = OpenTerrainRelease(root, f.asset, renamed.release);
    CHECK_FALSE(wrongRelease.value.has_value());
    CHECK(wrongRelease.error.code == Code::WrongIdentity);

    TerrainReleaseInfo otherAsset = f.info;
    otherAsset.asset = TerrainAsset("terrain/other.terrain.json");
    WriteRelease(root, otherAsset, encoded);
    const TerrainResult<TerrainReleaseHandle> wrongAsset = OpenTerrainRelease(root, otherAsset.asset, f.info.release);
    CHECK_FALSE(wrongAsset.value.has_value());
    CHECK(wrongAsset.error.code == Code::WrongIdentity);

    const TerrainResult<TerrainReleaseHandle> missing = OpenTerrainRelease(root, f.asset, TerrainReleaseId(0x42));
    CHECK_FALSE(missing.value.has_value());
    CHECK(missing.error.code == Code::Io);

    CHECK(OpenTerrainRelease(root, kNullAssetId, f.info.release).error.code == Code::InvalidInput);
    std::filesystem::remove_all(root);
}

TEST_CASE("A release opened before the pointer moves keeps reading its own files", "[terrain][release]")
{
    const std::filesystem::path root = MakeScratchDir("pointer");
    const Fixture f;
    const TerrainReleaseInfo infoA = Info(f.asset, 0xA);
    const TerrainReleaseInfo infoB = Info(f.asset, 0xB);
    const std::vector<TileHeights> tilesA = BuildTiles(f.layout, 0.0f);
    const std::vector<TileHeights> tilesB = BuildTiles(f.layout, 10.0f);

    CHECK(ReadTerrainCurrentPointer(root, f.asset).error.code == Code::Io);
    CHECK(OpenCurrentTerrainRelease(root, f.asset).error.code == Code::Io);

    WriteRelease(root, infoA, Encode(infoA, f.layout, tilesA, f.options));
    PointCurrentAt(root, f.asset, infoA.release);
    const TerrainReleaseHandle a = OpenOk(OpenCurrentTerrainRelease(root, f.asset));
    CHECK(a.Info().release == infoA.release);

    // B is published after A was opened, and the pointer moves to it.
    WriteRelease(root, infoB, Encode(infoB, f.layout, tilesB, f.options));
    PointCurrentAt(root, f.asset, infoB.release);

    const TerrainReleaseHandle b = OpenOk(OpenCurrentTerrainRelease(root, f.asset));
    CHECK(b.Info().release == infoB.release);
    CHECK(a.Info().release == infoA.release);
    for (size_t t = 0; t < tilesA.size(); ++t)
    {
        CHECK(ReadOk(a, tilesA[t].Tile()) == tilesA[t]);
        CHECK(ReadOk(b, tilesB[t].Tile()) == tilesB[t]);
    }
    CHECK(a.VerifyAllPacks().code == Code::None);

    const TerrainReleaseHandle copy = a;
    CHECK(copy.Info().release == infoA.release);
    CHECK(ReadOk(copy, {-2, -1}) == tilesA[0]);

    // A garbled pointer fails instead of falling back to some release.
    REQUIRE(WriteText(TerrainCurrentPointerPath(root, f.asset), "not a release"));
    CHECK(OpenCurrentTerrainRelease(root, f.asset).error.code == Code::Malformed);
    std::filesystem::remove_all(root);
}

TEST_CASE("Packs verify one at a time, and an asset's kept releases are listed with their sizes",
          "[terrain][release]")
{
    const std::filesystem::path root = MakeScratchDir("stored");
    const Fixture f;
    const TerrainReleaseInfo infoA = Info(f.asset, 0xA);
    const TerrainReleaseInfo infoB = Info(f.asset, 0xB);
    const TerrainEncodedRelease a = Encode(infoA, f.layout, BuildTiles(f.layout), f.options);
    const std::filesystem::path dirA = WriteRelease(root, infoA, a);
    WriteRelease(root, infoB, Encode(infoB, f.layout, BuildTiles(f.layout, 1.0f), f.options));
    PointCurrentAt(root, f.asset, infoB.release);
    std::filesystem::create_directories(TerrainAssetReleasesDir(root, f.asset) / "not-a-release");

    const TerrainReleaseHandle handle = OpenOk(OpenTerrainRelease(root, f.asset, infoA.release));
    REQUIRE(handle.Index().packs.size() == a.packs.size());
    REQUIRE(a.packs.size() > 1);
    for (size_t p = 0; p < handle.Index().packs.size(); ++p)
        CHECK(handle.VerifyPack(p).code == Code::None);
    CHECK(handle.VerifyPack(handle.Index().packs.size()).code == Code::InvalidInput);

    const std::string damaged = TerrainPackFileName(handle.Index().packs[1].chunk);
    std::optional<std::vector<std::byte>> bytes = ReadBytes(dirA / damaged);
    REQUIRE(bytes);
    bytes->back() ^= std::byte{0x40};
    REQUIRE(WriteBytes(dirA / damaged, *bytes));
    CHECK(handle.VerifyPack(0).code == Code::None);
    const TerrainPackError corrupt = handle.VerifyPack(1);
    CHECK(corrupt.code == Code::Corrupt);
    CHECK(Mentions(corrupt, damaged));
    CHECK(handle.VerifyAllPacks().code == Code::Corrupt);

    uint64_t bytesA = a.index.size();
    for (const TerrainEncodedPack &pack : a.packs)
        bytesA += pack.bytes.size();
    const std::vector<TerrainStoredRelease> stored = ListTerrainReleases(root, f.asset);
    REQUIRE(stored.size() == 2);
    CHECK(stored[0].release == infoA.release);
    CHECK(stored[0].files == a.packs.size() + 1);
    CHECK(stored[0].bytes == bytesA);
    CHECK(stored[1].release == infoB.release);
    CHECK(ListTerrainReleases(root, TerrainAsset("terrain/none.terrain.json")).empty());
    std::filesystem::remove_all(root);
}

// Run explicitly ("[.regenerate]") only after a deliberate format change.
TEST_CASE("Regenerate the committed terrain fixture", "[.regenerate]")
{
    const Fixture f;
    const TerrainEncodedRelease encoded = Encode(f.info, f.layout, BuildTiles(f.layout), f.options);
    const std::filesystem::path root = FixtureRoot();
    std::filesystem::remove_all(root);
    WriteRelease(root, f.info, encoded);
    REQUIRE(WriteText(TerrainCurrentPointerPath(root, f.asset), EncodeTerrainCurrentPointer(f.info.release)));
}

TEST_CASE("The committed fixture opens unchanged and matches the encoder byte for byte", "[terrain][release]")
{
    const Fixture f;
    const std::vector<TileHeights> tiles = BuildTiles(f.layout);
    const TerrainEncodedRelease encoded = Encode(f.info, f.layout, tiles, f.options);
    const std::filesystem::path root = FixtureRoot();
    const std::filesystem::path dir = TerrainReleaseDir(root, f.asset, f.info.release);

    REQUIRE(encoded.packs.size() == 4);
    CHECK(ReadBytes(dir / kTerrainIndexFileName) == encoded.index);
    for (const TerrainEncodedPack &pack : encoded.packs)
    {
        INFO(pack.fileName);
        CHECK(ReadBytes(dir / pack.fileName) == pack.bytes);
    }
    CHECK(ReadText(TerrainCurrentPointerPath(root, f.asset)) == EncodeTerrainCurrentPointer(f.info.release));

    TerrainReleaseHandle handle = OpenOk(OpenCurrentTerrainRelease(root, f.asset));
    CHECK(handle.Info() == f.info);
    CHECK(handle.VerifyAllPacks().code == Code::None);

    // One 100-byte payload out of a 260-byte, two-tile pack.
    handle.Source().ResetStats();
    CHECK(ReadOk(handle, {-1, -1}) == tiles[1]);
    CHECK(handle.Source().Stats().reads == 1);
    CHECK(handle.Source().Stats().bytes == 100);
    CHECK(encoded.index.size() == 1456);
    for (const TerrainEncodedPack &pack : encoded.packs)
        CHECK(pack.bytes.size() == 260);
}
