/**
 * @file TerrainBake.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainBake.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainBake.h>
#include <TerrainCandidate.h>
#include <TerrainGenerator.h>

#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    // Final = source in P1, so this follows the generator's pinned hash.
    constexpr uint64_t kExampleHeightsHash = 0x47af51a1d3ecb46eull;

    void CheckAllPassed(const TerrainReport &report)
    {
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        for (const TerrainCheck &c : report.checks)
        {
            INFO(c.name << ": " << c.detail);
            CHECK(c.status == CheckStatus::Passed);
        }
        CHECK(report.Ok());
    }
}

TEST_CASE("Bake final heights are the generator's source heights, tile by tile", "[terraintool][bake]")
{
    const TerrainRecipe recipe = LoadExample();
    TerrainReport report("bake", {check::kGenerate});
    const std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
    REQUIRE(tiles);
    CHECK(report.Status(check::kGenerate) == CheckStatus::Passed);

    const TerrainGenerator generator(recipe);
    const std::vector<TileCoord> order{{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
    REQUIRE(tiles->size() == order.size());
    for (size_t t = 0; t < order.size(); ++t)
    {
        const TileHeights &tile = (*tiles)[t];
        CHECK(tile.Tile() == order[t]);
        CHECK(tile.IntervalsPerTile() == 64);
        std::vector<float> source(TileSampleCount(recipe.layout));
        REQUIRE(generator.FillTile(order[t], source).Ok());
        CHECK(std::equal(source.begin(), source.end(), tile.Samples().begin(), tile.Samples().end(),
                         [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }));
    }
    CHECK(TerrainHeightsHash(*tiles) == kExampleHeightsHash);
}

TEST_CASE("A bake mints its release id once from the final heights and a nonce", "[terraintool][bake]")
{
    const TerrainRecipe recipe = LoadExample();
    const AssetId asset = MakeAssetId(ExampleAssetPath());
    const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
    TerrainReport report("bake", {check::kGenerate, check::kEncode});
    auto bake = [&](std::optional<uint64_t> nonce) {
        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        REQUIRE(tiles);
        std::optional<TerrainBakedRelease> baked =
            EncodeBakedRelease(asset, fingerprint, recipe.layout, std::move(*tiles), {nonce, {}}, report);
        REQUIRE(baked);
        return std::move(*baked);
    };

    const TerrainBakedRelease a = bake(kNonce);
    const TerrainBakedRelease b = bake(kNonce);
    const TerrainBakedRelease c = bake(std::nullopt);
    const TerrainBakedRelease d = bake(std::nullopt);
    CHECK(report.Ok());

    CHECK(a.info.release == MintTerrainReleaseId(asset, fingerprint, a.encoded.heightsHash, kNonce));
    CHECK(a.info.asset == asset);
    CHECK(a.info.source == fingerprint);
    CHECK(a.info.format == kTerrainFormatVersion);
    CHECK(a.encoded.index == b.encoded.index);
    REQUIRE(a.encoded.packs.size() == b.encoded.packs.size());
    for (size_t p = 0; p < a.encoded.packs.size(); ++p)
        CHECK(a.encoded.packs[p].bytes == b.encoded.packs[p].bytes);
    CHECK(c.encoded.heightsHash == a.encoded.heightsHash);
    CHECK(d.encoded.heightsHash == a.encoded.heightsHash);
    CHECK(c.info.release != a.info.release);
    CHECK(c.info.release != d.info.release);

    const TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(c.encoded));
    REQUIRE(opened);
    CHECK(opened.value->Info() == c.info);
    CHECK(opened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
    // Tiles straddling the origin fall in four chunks: one pack per tile.
    CHECK(a.encoded.packs.size() == 4);
}

TEST_CASE("A bake writes exactly the release files and a manifest, then nothing else", "[terraintool][bake]")
{
    ScratchDir scratch("bake");
    const fs::path out = scratch.path / "candidate";
    const TerrainReport report = RunTerrainBake(ExampleRequest(out));
    CheckAllPassed(report);

    CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    CHECK(FileNames(out) == std::vector<std::string>{"candidate.json", "chunk_-1_-1.tpack", "chunk_-1_0.tpack",
                                                     "chunk_0_-1.tpack", "chunk_0_0.tpack", "release.tindex"});

    REQUIRE(report.release);
    CHECK(report.release->asset == MakeAssetId(ExampleAssetPath()));
    CHECK(report.assetPath == ExampleAssetPath());
    CHECK(report.heightsHash == kExampleHeightsHash);
    CHECK(report.nonce == kNonce);
    CHECK(report.surfaceVersion == kTerrainSurfaceVersion);
    CHECK(report.candidateDir == out);
    REQUIRE(report.tiles.size() == 4);
    for (const TerrainTileSummary &tile : report.tiles)
        CHECK(tile.readable);
    REQUIRE(report.files.size() == 5);
    CHECK(report.files[0].name == "release.tindex");

    const std::optional<std::string> text = ReadText(out / kCandidateManifestFileName);
    REQUIRE(text);
    const ManifestParseResult manifest = ParseCandidateManifest(*text);
    REQUIRE(manifest.manifest);
    CHECK(manifest.manifest->release == *report.release);
    CHECK(manifest.manifest->assetPath == ExampleAssetPath());
    CHECK(manifest.manifest->heightsHash == report.heightsHash);
    CHECK(manifest.manifest->surfaceVersion == kTerrainSurfaceVersion);
    REQUIRE(manifest.manifest->files.size() == report.files.size());
    for (size_t f = 0; f < report.files.size(); ++f)
    {
        CHECK(manifest.manifest->files[f].name == report.files[f].name);
        CHECK(manifest.manifest->files[f].bytes == report.files[f].bytes);
        CHECK(manifest.manifest->files[f].checksum == report.files[f].checksum);
    }

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(out));
    REQUIRE(opened);
    CHECK(opened.value->Info() == *report.release);
}

TEST_CASE("A bake refuses an existing non-empty directory and leaves it as it was", "[terraintool][bake]")
{
    ScratchDir scratch("bake_refuse");
    const fs::path out = scratch.path / "candidate";
    fs::create_directories(out);
    const std::vector<std::byte> keep{std::byte{1}, std::byte{2}};
    WriteFile(out / "keep.bin", keep);

    const TerrainReport refused = RunTerrainBake(ExampleRequest(out));
    CHECK_FALSE(refused.Ok());
    const std::vector<TerrainDiagnostic> args = DiagnosticsOf(refused, check::kArguments);
    REQUIRE(args.size() == 1);
    CHECK(args[0].field == "--out");
    CHECK(refused.Status(check::kRecipe) == CheckStatus::Skipped);
    CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    CHECK(FileNames(out) == std::vector<std::string>{"keep.bin"});
    CHECK(ReadFile(out / "keep.bin") == keep);

    SECTION("an empty directory is filled")
    {
        fs::remove(out / "keep.bin");
        CheckAllPassed(RunTerrainBake(ExampleRequest(out)));
        CHECK(fs::exists(out / kCandidateManifestFileName));
        CHECK(FileNames(scratch.path) == std::vector<std::string>{"candidate"});
    }
    SECTION("a second bake into a finished candidate is refused")
    {
        const fs::path second = scratch.path / "second";
        CheckAllPassed(RunTerrainBake(ExampleRequest(second)));
        const auto before = Snapshot(second);
        CHECK_FALSE(RunTerrainBake(ExampleRequest(second, std::nullopt)).Ok());
        CHECK(Snapshot(second) == before);
    }
}

TEST_CASE("An invalid recipe or asset path fails before anything is written", "[terraintool][bake]")
{
    ScratchDir scratch("bake_invalid");
    const fs::path out = scratch.path / "candidate";

    SECTION("recipe fields")
    {
        std::ifstream in(ExampleRecipePath());
        Json recipe = Json::parse(in);
        recipe["parameters"]["broad"]["amplitude"] = -1.0;
        recipe.erase("seed");
        const fs::path path = scratch.path / "bad.terrain.json";
        REQUIRE(WriteText(path, recipe.dump(2)));
        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = path;

        const TerrainReport report = RunTerrainBake(request);
        CHECK_FALSE(report.Ok());
        std::vector<std::string> fields;
        for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kRecipe))
            fields.push_back(d.field);
        std::sort(fields.begin(), fields.end());
        CHECK(fields == std::vector<std::string>{"parameters.broad.amplitude", "seed"});
        CHECK(report.Status(check::kGenerate) == CheckStatus::Skipped);
        CHECK(report.Status(check::kWrite) == CheckStatus::Skipped);
    }
    SECTION("missing recipe file")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.recipePath = scratch.path / "absent.terrain.json";
        const TerrainReport report = RunTerrainBake(request);
        CHECK(report.Status(check::kRecipe) == CheckStatus::Failed);
    }
    SECTION("asset path that escapes its root")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.sourcePath = "../outside.terrain.json";
        const std::vector<TerrainDiagnostic> args = DiagnosticsOf(RunTerrainBake(request), check::kArguments);
        REQUIRE(args.size() == 1);
        CHECK(args[0].field == "--source-path");
    }
    SECTION("absolute id root")
    {
        TerrainBakeRequest request = ExampleRequest(out);
        request.idRoot = "C:/games/G/assets";
        const std::vector<TerrainDiagnostic> args = DiagnosticsOf(RunTerrainBake(request), check::kArguments);
        REQUIRE(args.size() == 1);
        CHECK(args[0].field == "--id-root");
    }
    CHECK_FALSE(fs::exists(out));
    for (const std::string &name : FileNames(scratch.path))
        CHECK(name.find(".partial") == std::string::npos);
}
