/**
 * @file TerrainToolCli.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/main.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainPublish.h>

#include <core/platform/Process.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <set>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    // FingerprintRecipe of tests/fixtures/relief-2x2.terrain.json.
    constexpr std::string_view kExampleFingerprint = "288219ec4e313cb7";

    int Run(const std::vector<std::string> &args)
    {
        const std::optional<int> code = LaunchProcess(TERRAIN_TOOL_EXE, args, true);
        REQUIRE(code);
        return *code;
    }

    std::vector<std::string> BakeArgs(const fs::path &out, const fs::path &report,
                                      std::optional<std::string> nonce = std::nullopt,
                                      const fs::path &recipe = ExampleRecipePath())
    {
        std::vector<std::string> args{"bake", "--recipe", recipe.string(), "--id-root", std::string(kIdRoot)};
        args.insert(args.end(), {"--source-path", std::string(kSourcePath), "--out", out.string()});
        args.insert(args.end(), {"--report", report.string()});
        if (nonce)
            args.insert(args.end(), {"--nonce", *nonce});
        return args;
    }

    std::vector<std::string> ValidateArgs(const fs::path &candidate, const fs::path &report)
    {
        return {"validate", "--candidate", candidate.string(), "--report",
                report.string(), "--recipe", ExampleRecipePath().string()};
    }

    Json ReadJson(const fs::path &path)
    {
        const std::optional<std::string> text = ReadText(path);
        REQUIRE(text);
        Json json = Json::parse(*text, nullptr, false);
        REQUIRE_FALSE(json.is_discarded());
        return json;
    }

    std::string CheckStatusOf(const Json &report, std::string_view name)
    {
        for (const Json &c : report.at("checks"))
            if (c.at("name").get<std::string>() == name)
                return c.at("status").get<std::string>();
        return "absent";
    }

    std::vector<Json> DiagnosticsOf(const Json &report, std::string_view check)
    {
        std::vector<Json> found;
        for (const Json &d : report.at("diagnostics"))
        {
            UNSCOPED_INFO(d.dump());
            if (d.at("check").get<std::string>() == check)
                found.push_back(d);
        }
        return found;
    }

    // Compact text, which Catch can print.
    std::string Text(const Json &json)
    {
        return json.dump();
    }

    std::string Tiles(std::initializer_list<TileCoord> tiles)
    {
        Json json = Json::array();
        for (TileCoord t : tiles)
            json.push_back({t.x, t.z});
        return json.dump();
    }

    std::string Sample(int64_t i, int64_t k)
    {
        return Json::array({i, k}).dump();
    }
}

TEST_CASE("TerrainTool bake writes a validated 2x2 candidate and a structured report", "[terraintool][cli]")
{
    ScratchDir scratch("cli_bake");
    const fs::path out = scratch.path / "build" / "terrain" / "candidate";
    const fs::path reportPath = scratch.path / "reports" / "bake.json";
    REQUIRE(Run(BakeArgs(out, reportPath)) == 0);

    CHECK(FileNames(out) == std::vector<std::string>{"candidate.json", "chunk_-1_-1.tpack", "chunk_-1_0.tpack",
                                                     "chunk_0_-1.tpack", "chunk_0_0.tpack", "release.tindex"});
    const Json report = ReadJson(reportPath);
    CHECK(report.at("kind") == "mir.terrain.report");
    CHECK(report.at("reportVersion") == kTerrainReportVersion);
    CHECK(report.at("tool").at("name") == "TerrainTool");
    CHECK(report.at("tool").at("version") == kTerrainToolVersion);
    CHECK(report.at("command") == "bake");
    CHECK(report.at("ok") == true);
    CHECK(report.at("recipe").at("path") == ExampleRecipePath().generic_string());
    CHECK(report.at("recipe").at("fingerprint") == std::string(kExampleFingerprint));
    CHECK(report.at("candidate") == out.generic_string());
    CHECK(report.at("asset").at("id") == TerrainIdHex(MakeAssetId(ExampleAssetPath()).value));
    CHECK(report.at("asset").at("logicalPath") == ExampleAssetPath());

    const Json &release = report.at("release");
    CHECK(release.at("id").get<std::string>().size() == 16);
    CHECK(release.at("sourceFingerprint") == std::string(kExampleFingerprint));
    CHECK(release.at("formatVersion") == kTerrainFormatVersion.value);
    CHECK(release.at("formatMinor") == kTerrainFormatMinor);
    CHECK(release.at("contentVersion") == kTerrainHeightsContentVersion);
    CHECK(release.at("surfaceVersion") == kTerrainSurfaceVersion);
    CHECK(release.at("heightsHash").get<std::string>().size() == 16);
    CHECK(report.at("layout").at("sampleSpacing") == 4.0);
    CHECK(report.at("layout").at("intervalsPerTile") == 64);

    REQUIRE(report.at("tiles").size() == 4);
    Json coords = Json::array();
    for (const Json &tile : report.at("tiles"))
    {
        coords.push_back(tile.at("tile"));
        CHECK(tile.at("readable") == true);
    }
    CHECK(Text(coords) == Tiles({{-1, -1}, {0, -1}, {-1, 0}, {0, 0}}));
    CHECK(report.at("files").size() == 5);
    for (const Json &c : report.at("checks"))
    {
        INFO(c.dump());
        CHECK(c.at("status") == "passed");
    }
    CHECK(report.at("diagnostics").empty());

    const TerrainResult<TerrainReleaseHandle> opened =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(out));
    REQUIRE(opened);
    CHECK(release.at("id") == TerrainIdHex(opened.value->Info().release.value));
    CHECK(release.at("heightsHash") == TerrainIdHex(opened.value->Index().heightsHash));

    const fs::path validatePath = scratch.path / "reports" / "validate.json";
    REQUIRE(Run(ValidateArgs(out, validatePath)) == 0);
    const Json validated = ReadJson(validatePath);
    CHECK(validated.at("command") == "validate");
    CHECK(validated.at("ok") == true);
    CHECK(Text(validated.at("release")) == Text(release));
    CHECK(CheckStatusOf(validated, "recipe") == "passed");
    CHECK(CheckStatusOf(validated, "identity") == "passed");
}

TEST_CASE("Repeated bakes give the same final heights; only a fixed nonce repeats the release", "[terraintool][cli]")
{
    ScratchDir scratch("cli_repeat");
    auto bake = [&](const char *name, std::optional<std::string> nonce) {
        const fs::path out = scratch.path / name;
        const fs::path report = scratch.path / (std::string(name) + ".json");
        REQUIRE(Run(BakeArgs(out, report, nonce)) == 0);
        return ReadJson(report).at("release");
    };

    const Json a = bake("a", std::nullopt);
    const Json b = bake("b", std::nullopt);
    CHECK(Text(a.at("heightsHash")) == Text(b.at("heightsHash")));
    CHECK(Text(a.at("id")) != Text(b.at("id")));
    CHECK(Text(a.at("nonce")) != Text(b.at("nonce")));

    const TerrainResult<TerrainReleaseHandle> ra =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(scratch.path / "a"));
    const TerrainResult<TerrainReleaseHandle> rb =
        TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(scratch.path / "b"));
    REQUIRE(ra);
    REQUIRE(rb);
    for (const TerrainTileRecord &record : ra.value->Index().tiles)
    {
        const TerrainResult<TileHeights> ta = ra.value->ReadTile({ra.value->Info().asset, record.tile});
        const TerrainResult<TileHeights> tb = rb.value->ReadTile({rb.value->Info().asset, record.tile});
        REQUIRE(ta);
        REQUIRE(tb);
        CHECK(*ta.value == *tb.value);
    }

    const Json c = bake("c", "0123456789abcdef");
    const Json d = bake("d", "0123456789abcdef");
    CHECK(Text(c) == Text(d));
    CHECK(Text(c.at("heightsHash")) == Text(a.at("heightsHash")));
    CHECK(c.at("nonce") == "0123456789abcdef");
    CHECK(Snapshot(scratch.path / "c") == Snapshot(scratch.path / "d"));
}

TEST_CASE("TerrainTool fails bad recipes and damaged candidates with tiles and fields named", "[terraintool][cli]")
{
    ScratchDir scratch("cli_fail");
    const fs::path reportPath = scratch.path / "report.json";
    const fs::path baked = scratch.path / "baked";
    const fs::path crafted = scratch.path / "crafted";

    SECTION("invalid recipe")
    {
        std::ifstream in(ExampleRecipePath());
        Json recipe = Json::parse(in);
        recipe["layout"]["intervalsPerTile"] = 2.5;
        recipe["parameters"]["ridges"]["sharpness"] = 3;
        const fs::path bad = scratch.path / "bad.terrain.json";
        REQUIRE(WriteText(bad, recipe.dump(2)));

        CHECK(Run(BakeArgs(baked, reportPath, std::nullopt, bad)) == 1);
        const Json report = ReadJson(reportPath);
        CHECK(report.at("ok") == false);
        CHECK(CheckStatusOf(report, "recipe") == "failed");
        CHECK(CheckStatusOf(report, "write") == "skipped");
        std::set<std::string> fields;
        for (const Json &d : DiagnosticsOf(report, "recipe"))
            fields.insert(d.at("field").get<std::string>());
        CHECK(fields == std::set<std::string>{"layout.intervalsPerTile", "parameters.ridges.sharpness"});
        CHECK_FALSE(fs::exists(baked));
        CHECK(FileNames(scratch.path) == std::vector<std::string>{"bad.terrain.json", "report.json"});
    }
    SECTION("missing tile")
    {
        REQUIRE(Run(BakeArgs(baked, scratch.path / "bake.json")) == 0);
        fs::remove(baked / Locate(baked, {-1, 0}).pack);
        CHECK(Run(ValidateArgs(baked, reportPath)) == 1);
        const Json report = ReadJson(reportPath);
        CHECK(report.at("ok") == false);
        const std::vector<Json> coverage = DiagnosticsOf(report, "coverage");
        REQUIRE(coverage.size() == 1);
        CHECK(Text(coverage[0].at("tiles")) == Tiles({{-1, 0}}));
        CHECK(coverage[0].at("field") == "chunk_-1_0.tpack");
        CHECK(CheckStatusOf(report, "edges") == "skipped");
    }
    SECTION("corrupt payload")
    {
        REQUIRE(Run(BakeArgs(baked, scratch.path / "bake.json")) == 0);
        const TileLocation where = Locate(baked, {0, 0});
        std::vector<std::byte> pack = ReadFile(baked / where.pack);
        pack[size_t(where.offset + where.length / 2)] ^= std::byte{0x10};
        WriteFile(baked / where.pack, pack);
        CHECK(Run(ValidateArgs(baked, reportPath)) == 1);
        const Json report = ReadJson(reportPath);
        const std::vector<Json> payloads = DiagnosticsOf(report, "payloads");
        REQUIRE(payloads.size() == 1);
        CHECK(Text(payloads[0].at("tiles")) == Tiles({{0, 0}}));
        CHECK(payloads[0].at("field") == "payload");
        CHECK(CheckStatusOf(report, "packs") == "failed");
    }
    SECTION("non-finite height")
    {
        REQUIRE(Run(BakeArgs(baked, scratch.path / "bake.json")) == 0);
        PatchSampleSealed(baked, {-1, -1}, 40, 3, std::numeric_limits<float>::infinity());
        CHECK(Run(ValidateArgs(baked, reportPath)) == 1);
        const Json report = ReadJson(reportPath);
        const std::vector<Json> finite = DiagnosticsOf(report, "finite");
        REQUIRE(finite.size() == 1);
        CHECK(Text(finite[0].at("tiles")) == Tiles({{-1, -1}}));
        CHECK(finite[0].at("field") == "samples");
        CHECK(Text(finite[0].at("sample")) == Sample(-64 + 40, -64 + 3));
    }
    SECTION("edge discontinuity")
    {
        WriteCraftedCandidate(crafted, [](std::vector<TileHeights> &tiles) {
            TileHeights &tile = TileAt(tiles, {-1, 0});
            tile.Set(30, 0, tile.At(30, 0) - 2.0f);
        });
        CHECK(Run(ValidateArgs(crafted, reportPath)) == 1);
        const Json report = ReadJson(reportPath);
        CHECK(CheckStatusOf(report, "edges") == "failed");
        CHECK(CheckStatusOf(report, "corners") == "passed");
        const std::vector<Json> edges = DiagnosticsOf(report, "edges");
        REQUIRE(edges.size() == 2);
        CHECK(edges[0].at("field") == "samples");
        CHECK(Text(edges[0].at("tiles")) == Tiles({{-1, -1}, {-1, 0}}));
        CHECK(Text(edges[0].at("sample")) == Sample(-64 + 30, 0));
        CHECK(edges[1].at("field") == "surface");
    }
    SECTION("corner discontinuity")
    {
        WriteCraftedCandidate(crafted, [](std::vector<TileHeights> &tiles) {
            TileHeights &tile = TileAt(tiles, {-1, -1});
            tile.Set(64, 64, tile.At(64, 64) + 1.0f);
        });
        CHECK(Run(ValidateArgs(crafted, reportPath)) == 1);
        const Json report = ReadJson(reportPath);
        const std::vector<Json> corners = DiagnosticsOf(report, "corners");
        REQUIRE(corners.size() == 2);
        for (const Json &d : corners)
        {
            CHECK(Text(d.at("tiles")) == Tiles({{-1, -1}, {0, -1}, {-1, 0}, {0, 0}}));
            CHECK(Text(d.at("sample")) == Sample(0, 0));
        }
    }
}

TEST_CASE("Bake and validate never change a published release or its current pointer", "[terraintool][cli]")
{
    ScratchDir scratch("cli_published");
    const fs::path releases = scratch.path / "releases";
    const AssetId asset = MakeAssetId(ExampleAssetPath());

    const fs::path staged = scratch.path / "staged";
    const TerrainBakedRelease published = WriteCraftedCandidate(staged, [](auto &) {}, 0xC0FFEE);
    const fs::path releaseDir = TerrainReleaseDir(releases, asset, published.info.release);
    fs::create_directories(releaseDir);
    WriteFile(releaseDir / kTerrainIndexFileName, published.encoded.index);
    for (const TerrainEncodedPack &pack : published.encoded.packs)
        WriteFile(releaseDir / pack.fileName, pack.bytes);
    REQUIRE(WriteTextAtomic(TerrainCurrentPointerPath(releases, asset),
                            EncodeTerrainCurrentPointer(published.info.release)));
    const auto before = Snapshot(releases);
    REQUIRE(before.size() == 6);

    const fs::path report = scratch.path / "report.json";
    const fs::path good = scratch.path / "good";
    CHECK(Run(BakeArgs(good, report)) == 0);
    CHECK(Run(ValidateArgs(good, report)) == 0);

    const fs::path bad = scratch.path / "bad.terrain.json";
    REQUIRE(WriteText(bad, "{ \"recipeVersion\": 1 }"));
    CHECK(Run(BakeArgs(scratch.path / "never", report, std::nullopt, bad)) == 1);
    CHECK(Run(BakeArgs(TerrainAssetReleasesDir(releases, asset), report)) == 1);
    CHECK(Run(BakeArgs(releaseDir, report, TerrainIdHex(0xC0FFEE))) == 1);
    CHECK(Run(BakeArgs(releases, report)) == 1);
    // No candidate.json, so not a candidate; reading it changes nothing.
    CHECK(Run(ValidateArgs(releaseDir, report)) == 1);
    CHECK(DiagnosticsOf(ReadJson(report), "candidate").size() == 1);
    fs::remove(good / Locate(good, {0, 0}).pack);
    CHECK(Run(ValidateArgs(good, report)) == 1);

    CHECK(Snapshot(releases) == before);
    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(releases, asset);
    REQUIRE(current);
    CHECK(current.value->Info() == published.info);
    CHECK_FALSE(fs::exists(scratch.path / "never"));
}

TEST_CASE("Bad command lines exit 2 and still leave a report", "[terraintool][cli]")
{
    ScratchDir scratch("cli_usage");
    const fs::path report = scratch.path / "usage.json";
    const fs::path out = scratch.path / "out";

    CHECK(Run({}) == 2);
    CHECK(Run({"--help"}) == 0);
    CHECK(Run({"publish", "--report", report.string()}) == 2);
    CHECK(ReadJson(report).at("command") == "publish");

    std::vector<std::string> noOut{"bake",          "--recipe",   ExampleRecipePath().string(), "--id-root",
                                   std::string(kIdRoot), "--source-path", std::string(kSourcePath), "--report",
                                   report.string()};
    CHECK(Run(noOut) == 2);
    const Json missing = ReadJson(report);
    CHECK(missing.at("ok") == false);
    CHECK(CheckStatusOf(missing, "arguments") == "failed");
    REQUIRE(missing.at("diagnostics").size() == 1);
    CHECK(missing.at("diagnostics")[0].at("message").get<std::string>().find("--out") != std::string::npos);

    std::vector<std::string> args = BakeArgs(out, report);
    args.push_back("--releases");
    args.push_back(scratch.path.string());
    CHECK(Run(args) == 2);
    const fs::path inside = scratch.path / "inside";
    CHECK(Run(BakeArgs(inside, inside / "report.json")) == 2);
    // A refused report path is not written either, so --out is never created.
    CHECK_FALSE(fs::exists(inside));
    CHECK(Run(BakeArgs(out, report, "not-hex")) == 2);
    std::vector<std::string> twice = BakeArgs(out, report);
    twice.push_back("--out");
    twice.push_back(out.string());
    CHECK(Run(twice) == 2);
    CHECK(Run({"validate", "--report", report.string()}) == 2);
    CHECK_FALSE(fs::exists(out));

    const fs::path good = scratch.path / "good";
    REQUIRE(Run(BakeArgs(good, report)) == 0);
    CHECK(Run(ValidateArgs(good, good / "v.json")) == 2);
    CHECK_FALSE(fs::exists(good / "v.json"));
    CHECK(Run(ValidateArgs(good, report)) == 0);
}

TEST_CASE("TerrainTool publish and sync report what they did, with exit codes", "[terraintool][cli][publish]")
{
    ScratchDir scratch("cli_publish");
    const fs::path candidate = scratch.path / "build" / "candidate";
    const fs::path releases = scratch.path / "releases";
    const fs::path reportPath = scratch.path / "reports" / "publish.json";
    const AssetId asset = MakeAssetId(ExampleAssetPath());
    REQUIRE(Run(BakeArgs(candidate, scratch.path / "reports" / "bake.json")) == 0);
    const std::vector<std::string> publish{"publish",  "--candidate", candidate.string(), "--releases",
                                           releases.string(), "--report", reportPath.string(), "--recipe",
                                           ExampleRecipePath().string()};

    REQUIRE(Run(publish) == 0);
    const Json report = ReadJson(reportPath);
    CHECK(report.at("command") == "publish");
    CHECK(report.at("ok") == true);
    const std::string id = report.at("release").at("id").get<std::string>();
    const Json &summary = report.at("publish");
    CHECK(summary.at("releases") == releases.generic_string());
    CHECK(summary.at("previous").is_null());
    CHECK(summary.at("current") == id);
    CHECK(summary.at("published") == true);
    for (const char *name : {"recipe", "identity", "lock", "target", "copy", "verify", "rename", "pointer"})
    {
        INFO(name);
        CHECK(CheckStatusOf(report, name) == "passed");
    }
    CHECK(ReadText(TerrainCurrentPointerPath(releases, asset)) == id);

    const auto tree = Snapshot(releases);
    CHECK(Run(publish) == 1);
    const Json again = ReadJson(reportPath);
    CHECK(again.at("ok") == false);
    CHECK(DiagnosticsOf(again, "target").size() == 1);
    CHECK(again.at("publish").at("current") == id);
    CHECK(again.at("publish").at("published") == false);
    CHECK(Snapshot(releases) == tree);

    // Another process holds the asset lock: fails at once.
    const fs::path second = scratch.path / "build" / "second";
    REQUIRE(Run(BakeArgs(second, scratch.path / "reports" / "bake2.json")) == 0);
    {
        std::string error;
        const std::optional<TerrainPublishLock> held =
            TerrainPublishLock::Acquire(TerrainAssetReleasesDir(releases, asset), std::chrono::milliseconds(0), error);
        REQUIRE(held);
        CHECK(Run({"publish", "--candidate", second.string(), "--releases", releases.string(), "--report",
                   reportPath.string()}) == 1);
        CHECK(CheckStatusOf(ReadJson(reportPath), "lock") == "failed");
    }
    CHECK(Snapshot(releases) == tree);

    // Usage errors exit 2 and never leave a report inside the release tree.
    CHECK(Run({"publish", "--candidate", second.string(), "--report", reportPath.string()}) == 2);
    CHECK(Run({"publish", "--candidate", second.string(), "--releases", releases.string(), "--report",
               (releases / "report.json").string()}) == 2);
    CHECK(Run({"publish", "--candidate", second.string(), "--releases", releases.string(), "--report",
               (second / "report.json").string()}) == 2);
    CHECK(Snapshot(releases) == tree);
}

TEST_CASE("TerrainTool sync publishes into the game's releases once per recipe change", "[terraintool][cli][sync]")
{
    ScratchDir scratch("cli_sync");
    const fs::path gameDir = scratch.path / "games" / "G";
    const fs::path recipe = gameDir / "assets" / "terrain" / "relief-2x2.terrain.json";
    fs::create_directories(recipe.parent_path());
    fs::copy_file(ExampleRecipePath(), recipe);
    const fs::path reportPath = scratch.path / "build" / "terrain" / "relief-2x2.sync.json";
    const std::vector<std::string> sync{"sync",          "--recipe",     recipe.string(),
                                        "--id-root",     std::string(kIdRoot), "--source-path",
                                        std::string(kSourcePath), "--game-dir", gameDir.string(),
                                        "--candidate",   (scratch.path / "build" / "terrain" / "relief-2x2").string(),
                                        "--report",      reportPath.string()};
    const fs::path releases = TerrainReleasesRoot(gameDir);
    const fs::path assetDir = TerrainAssetReleasesDir(releases, MakeAssetId(ExampleAssetPath()));

    REQUIRE(Run(sync) == 0);
    const Json first = ReadJson(reportPath);
    CHECK(first.at("command") == "sync");
    CHECK(first.at("publish").at("published") == true);
    CHECK(first.at("publish").at("releases") == releases.generic_string());
    CHECK(first.at("asset").at("logicalPath") == ExampleAssetPath());
    const std::string id = first.at("publish").at("current").get<std::string>();
    const auto tree = Snapshot(releases);

    REQUIRE(Run(sync) == 0);
    const Json second = ReadJson(reportPath);
    CHECK(second.at("ok") == true);
    CHECK(second.at("publish").at("published") == false);
    CHECK(second.at("publish").at("current") == id);
    CHECK(second.at("release").at("id") == id);
    CHECK(second.at("asset").at("id") == TerrainIdHex(MakeAssetId(ExampleAssetPath()).value));
    CHECK(CheckStatusOf(second, "current") == "passed");
    CHECK(CheckStatusOf(second, "generate") == "skipped");
    CHECK(Snapshot(releases) == tree);
    CHECK(ReleaseDirNames(assetDir) == std::vector<std::string>{id});

    WriteSeedRecipe(recipe.parent_path(), 11, recipe.filename().string());
    REQUIRE(Run(sync) == 0);
    const Json changed = ReadJson(reportPath);
    CHECK(changed.at("publish").at("published") == true);
    CHECK(changed.at("publish").at("previous") == id);
    CHECK(ReleaseDirNames(assetDir).size() == 2);

    std::vector<std::string> broken = sync;
    broken[2] = (recipe.parent_path() / "absent.terrain.json").string();
    CHECK(Run(broken) == 1);
    CHECK(CheckStatusOf(ReadJson(reportPath), "recipe") == "failed");
    std::vector<std::string> missing(sync.begin(), sync.begin() + 7);
    missing.insert(missing.end(), {"--report", reportPath.string()});
    CHECK(Run(missing) == 2);
    CHECK(ReleaseDirNames(assetDir).size() == 2);
}
