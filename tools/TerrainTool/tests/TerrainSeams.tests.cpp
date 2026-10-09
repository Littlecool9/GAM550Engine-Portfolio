#include "TerrainBakeTestSupport.h"
#include <TerrainContinuity.h>
#include <TerrainGenerator.h>
#include <TerrainPublish.h>
#include <TerrainSeams.h>
#include <TerrainSync.h>
#include <TerrainValidate.h>
#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainTileMesh.h>

#include <bit>
#include <cmath>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    Json SeamJson()
    {
        const auto text = ReadText(fs::path(TERRAIN_FIXTURE_DIR) / "mixed-2x2.terrain.json");
        REQUIRE(text);
        Json j = Json::parse(*text);
        j["layout"]["intervalsPerTile"] = 64;
        j["layout"]["bounds"] = {{"minX", -61.5}, {"minZ", -64.0}, {"maxX", 64.0}, {"maxZ", 59.25}};
        return j;
    }
    TerrainRecipe Recipe(const Json &j)
    {
        const auto parsed = ParseTerrainRecipe(j.dump());
        for (const auto &i : parsed.issues) UNSCOPED_INFO(i.field << ": " << i.message);
        REQUIRE(parsed.recipe);
        return *parsed.recipe;
    }
    std::vector<TileHeights> Raw(const TerrainRecipe &recipe)
    {
        const TerrainGenerator generator(recipe);
        std::vector<TileHeights> out;
        const auto range = recipe.layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z) for (int32_t x = range.min.x; x <= range.max.x; ++x)
        {
            const auto layout = *recipe.SourceLayout({x, z});
            auto tile = TileHeights::Create({x, z}, layout.IntervalsPerTile());
            REQUIRE(tile);
            REQUIRE(generator.FillTile(layout, {x, z}, tile->Samples()).Ok());
            out.push_back(std::move(*tile));
        }
        return out;
    }
    ContinuityStats Check(const TerrainRecipe &recipe, const std::vector<TileHeights> &tiles, TerrainReport &report)
    {
        const auto layers = recipe.SourceLayers();
        return CheckMixedTileContinuity(recipe.layout, [&](TileCoord t) {
            const auto tile = std::find_if(tiles.begin(), tiles.end(), [&](const auto &v) { return v.Tile() == t; });
            if (tile == tiles.end()) return TerrainContinuityTile{};
            const auto l = std::find_if(layers.begin(), layers.end(), [&](const auto &v) { return v.IntervalsPerTile() == tile->IntervalsPerTile(); });
            return TerrainContinuityTile{&*l, tile->Samples()};
        }, report);
    }
    TerrainSourceFingerprint BeforeSeamFingerprint(const TerrainRecipe &r)
    {
        auto text = CanonicalRecipeText(r);
        for (const std::string prefix : {"finalSeams.version=", "finalSeams.bandMultiplier=", "finalSeams.weight=", "derivatives.version=", "derivatives.maxSpacing="})
        {
            const auto pos = text.find(prefix); REQUIRE(pos != std::string::npos);
            text.erase(pos, text.find('\n', pos) - pos + 1);
        }
        const auto pos = text.find("canonicalVersion=4"); REQUIRE(pos != std::string::npos);
        text.replace(pos, 18, "canonicalVersion=2");
        return TerrainSourceFingerprint(Fnv1a64(text));
    }
    TerrainBakedRelease Write(const fs::path &dir, const TerrainRecipe &recipe, std::vector<TileHeights> tiles,
                              TerrainReport &report, uint64_t nonce)
    {
        auto b = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe, std::move(tiles), {nonce, {}}, report);
        REQUIRE(b);
        fs::create_directories(dir);
        std::string error;
        REQUIRE(WriteCandidateFiles(dir, b->encoded, MakeCandidateManifest(*b, ExampleAssetPath(), "test"), error));
        return std::move(*b);
    }
}

TEST_CASE("Final seams cover direct 1 to 2 4 8 borders and clipped corners", "[terraintool][seams]")
{
    for (double spacing : {2.0, 4.0, 8.0})
    {
        auto j = SeamJson();
        j["precision"]["tiles"][1]["spacing"] = spacing;
        const auto r = Recipe(j);
        TerrainReport report("seams", {});
        auto tiles = Raw(r);
        auto before = tiles;
        REQUIRE(AdjustTerrainSeams(r, tiles, report));
        const auto stats = Check(r, tiles, report);
        for (const auto &d : report.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
        CHECK(report.Ok());
        CHECK(stats.edgesCompared == 4);
        CHECK(stats.cornersCompared == 1);
        CHECK(stats.breakpoints > 120);
        CHECK(stats.probes > 480);
        CHECK(report.seams->maxAdjustment > 0.0);
        CHECK(TileAt(tiles, {-1, -1}).At(32, 32) == TileAt(before, {-1, -1}).At(32, 32));
        const float corner = TileAt(tiles, {-1, -1}).At(64, 64);
        CHECK(corner == TileAt(tiles, {0, -1}).At(0, int32_t(64 / spacing)));
        CHECK(corner == TileAt(tiles, {-1, 0}).At(32, 0));
        CHECK(corner == TileAt(tiles, {0, 0}).At(0, 0));
    }
}

TEST_CASE("Same precision edges inherit the coarse incident corner through their first segment", "[terraintool][seams]")
{
    auto j = SeamJson();
    j["precision"]["tiles"][1]["spacing"] = 1.0;
    j["precision"]["tiles"][2]["spacing"] = 1.0;
    j["precision"]["tiles"][3]["spacing"] = 8.0;
    const auto r = Recipe(j);
    auto tiles = Raw(r);
    TerrainReport report("corner", {});
    REQUIRE(AdjustTerrainSeams(r, tiles, report));
    const auto stats = Check(r, tiles, report);
    CHECK(report.Ok());
    CHECK(stats.edgesCompared == 4);
    CHECK(TileAt(tiles, {-1, -1}).At(64, 63) == TileAt(tiles, {0, -1}).At(0, 63));
    CHECK(TileAt(tiles, {-1, -1}).At(64, 64) == TileAt(tiles, {0, -1}).At(0, 64));
    CHECK(TileAt(tiles, {-1, -1}).At(63, 64) == TileAt(tiles, {-1, 0}).At(63, 0));
}

TEST_CASE("Final seam adjustments and encoded releases do not depend on tile order", "[terraintool][seams]")
{
    const auto r = Recipe(SeamJson());
    auto a = Raw(r), b = a;
    std::reverse(b.begin(), b.end());
    TerrainReport ra("a", {}), rb("b", {});
    REQUIRE(AdjustTerrainSeams(r, a, ra)); REQUIRE(AdjustTerrainSeams(r, b, rb));
    auto ea = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(r), r, a, {kNonce, {}}, ra);
    auto eb = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(r), r, b, {kNonce, {}}, rb);
    REQUIRE(ea); REQUIRE(eb);
    CHECK(ea->info == eb->info);
    CHECK(ea->encoded.index == eb->encoded.index);
    for (size_t p = 0; p < ea->encoded.packs.size(); ++p) CHECK(ea->encoded.packs[p].bytes == eb->encoded.packs[p].bytes);
    CHECK(ra.seams->maxAdjustment == rb.seams->maxAdjustment);
}

TEST_CASE("Final seams preserve signed zero at exact shared source nodes", "[terraintool][seams][regression]")
{
    for (double base : {-1e-50, 1e-50}) for (double spacing : {1.0, 2.0})
    {
        INFO("base " << base << ", neighbour spacing " << spacing);
        auto json = SeamJson();
        json["layout"]["intervalsPerTile"] = 8;
        json["layout"]["bounds"] = {{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 16.0}, {"maxZ", 8.0}};
        json["precision"] = {{"defaultSpacing", 1.0},
            {"tiles", Json::array({{{"x", 1}, {"z", 0}, {"spacing", spacing}}})}};
        json["parameters"]["baseHeight"] = base;
        json["parameters"]["tilt"] = {{"x", 0.0}, {"z", 0.0}};
        for (auto term : {"broad", "ridges", "detail"}) json["parameters"][term]["amplitude"] = 0.0;
        json["parameters"]["plains"] = {{"height", -1.0}, {"blend", 0.0}};
        const auto recipe = Recipe(json);
        auto tiles = Raw(recipe);
        const auto coarseBefore = TileAt(tiles, {1, 0});
        REQUIRE(std::signbit(coarseBefore.At(0, 1)) == (base < 0.0));
        TerrainReport report("signed_zero", {});
        REQUIRE(AdjustTerrainSeams(recipe, tiles, report));
        const auto &fine = TileAt(tiles, {0, 0});
        const auto &coarse = TileAt(tiles, {1, 0});
        for (int32_t k = 0; k <= coarse.IntervalsPerTile(); ++k)
        {
            const auto expected = std::bit_cast<uint32_t>(coarseBefore.At(0, k));
            CHECK(std::bit_cast<uint32_t>(fine.At(8, k * int32_t(spacing))) == expected);
            CHECK(std::bit_cast<uint32_t>(coarse.At(0, k)) == expected);
        }
        const auto stats = Check(recipe, tiles, report);
        for (const auto &d : report.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
        CHECK(stats.edgesCompared == 1);
        CHECK(report.Ok());
    }
}

TEST_CASE("Sparse height layers compare baked diagonal tiles at their shared corner", "[terraintool][continuity][sparse][regression]")
{
    const auto layout = TerrainLayout::Create(2.0, 1, {0.0, 0.0, 4.0, 4.0});
    REQUIRE(layout);
    auto lo = TileHeights::Create({0, 0}, 1);
    auto hi = TileHeights::Create({1, 1}, 1);
    REQUIRE(lo); REQUIRE(hi);
    bool secondBaked = true, secondReadable = true;
    bool differs = false;
    SECTION("equal shared corner") {}
    SECTION("different heights") { hi->Set(0, 0, 5.0f); differs = true; }
    SECTION("different zero signs") { lo->Set(1, 1, -0.0f); differs = true; }
    SECTION("a baked payload is unreadable") { secondReadable = false; }
    SECTION("a single baked tile has no corner neighbour") { secondBaked = false; }
    const auto baked = [&](TileCoord tile) {
        return tile == lo->Tile() || (secondBaked && tile == hi->Tile());
    };
    const TileSampleLookup lookup = [&](TileCoord tile) -> std::span<const float> {
        if (tile == lo->Tile()) return lo->Samples();
        if (tile == hi->Tile() && secondReadable) return hi->Samples();
        return {};
    };
    TerrainReport report("sparse_corner", {check::kEdges, check::kCorners});
    const auto stats = CheckTileContinuity(*layout, lookup, report, baked);
    CHECK(stats.edgesCompared == 0);
    CHECK(stats.corners == (secondBaked ? 1 : 0));
    CHECK(stats.cornersCompared == (secondBaked && secondReadable ? 1 : 0));
    if (!secondReadable)
        CHECK(report.Status(check::kCorners) == CheckStatus::Skipped);
    else if (differs)
    {
        CHECK(report.Status(check::kCorners) == CheckStatus::Failed);
        const auto samples = std::find_if(report.diagnostics.begin(), report.diagnostics.end(), [](const auto &d) {
            return d.check == check::kCorners && d.field == "samples";
        });
        REQUIRE(samples != report.diagnostics.end());
        CHECK(samples->tiles == std::vector<TileCoord>{{0, 0}, {1, 1}});
        CHECK(samples->sample == GlobalSampleIndex{1, 1});
    }
    else
        CHECK(report.Status(check::kCorners) == CheckStatus::Passed);
}

TEST_CASE("Seam tolerance uses interpolation supports when a steep edge crosses zero", "[terraintool][seams]")
{
    auto j = SeamJson();
    const auto r = Recipe(j);
    auto tiles = Raw(r);
    for (auto &tile : tiles)
        for (int32_t k = 0; k <= tile.IntervalsPerTile(); ++k)
            for (int32_t i = 0; i <= tile.IntervalsPerTile(); ++i)
                tile.Set(i, k, float((double(k) / tile.IntervalsPerTile() - 0.5) * 19000.0));
    TerrainReport report("zero", {});
    REQUIRE(AdjustTerrainSeams(r, tiles, report));
    const auto stats = Check(r, tiles, report);
    for (const auto &d : report.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
    CHECK(report.Ok());
    CHECK(stats.maxTolerance > 0.0001);
    CHECK(stats.maxSurfaceDifference < stats.maxTolerance);
    CHECK(TerrainSeamTolerance(1000.0, 0.0, 1.0) > SurfaceTolerance(0.0));
}

TEST_CASE("Discontinuous final candidate cannot replace a published release", "[terraintool][seams]")
{
    ScratchDir scratch("seam_gate");
    const auto r = Recipe(SeamJson());
    TerrainReport good("good", {});
    auto tiles = BakeFinalHeights(r, good); REQUIRE(tiles);
    const auto first = Write(scratch.path / "good", r, *tiles, good, kNonce);
    const auto published = RunTerrainPublish({scratch.path / "good", scratch.path / "releases"});
    REQUIRE(published.Ok());
    TileAt(*tiles, {-1, -1}).Set(64, 31, TileAt(*tiles, {-1, -1}).At(64, 31) + 0.02f);
    TerrainReport bad("bad", {}); bad.seams = good.seams;
    Write(scratch.path / "bad", r, std::move(*tiles), bad, kNonce + 1);
    const auto rejected = RunTerrainPublish({scratch.path / "bad", scratch.path / "releases"});
    CHECK_FALSE(rejected.Ok());
    CHECK(rejected.Status(check::kEdges) == CheckStatus::Failed);
    CHECK(PeekTerrainCurrent(scratch.path / "releases", first.info.asset) == first.info.release);
}

TEST_CASE("Excessive continuous height adjustment warns and remains publishable", "[terraintool][seams]")
{
    ScratchDir scratch("seam_warning");
    auto j = SeamJson(); j["parameters"]["detail"]["amplitude"] = 500.0;
    const auto r = Recipe(j);
    TerrainReport report("large", {});
    auto tiles = BakeFinalHeights(r, report); REQUIRE(tiles);
    REQUIRE(report.seams); CHECK(report.seams->maxAdjustment > kTerrainSeamWarningThreshold);
    CHECK_FALSE(report.warnings.empty()); CHECK(report.diagnostics.empty()); CHECK(report.Ok());
    const auto baked = Write(scratch.path / "candidate", r, std::move(*tiles), report, kNonce);
    const auto published = RunTerrainPublish({scratch.path / "candidate", scratch.path / "releases"});
    for (const auto &d : published.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
    CHECK(published.Ok()); CHECK_FALSE(published.warnings.empty());
    CHECK(published.publish->published);
    CHECK(PeekTerrainCurrent(scratch.path / "releases", baked.info.asset) == baked.info.release);
    const auto json = Json::parse(TerrainReportJson(published));
    CHECK(json["ok"] == true); CHECK_FALSE(json["warnings"].empty());
    CHECK(json["seams"]["maxAdjustment"].get<double>() == report.seams->maxAdjustment);
}

TEST_CASE("Query and collider sampling consume adjusted release values", "[terraintool][seams]")
{
    const auto r = Recipe(SeamJson()); TerrainReport report("query", {});
    auto tiles = BakeFinalHeights(r, report); REQUIRE(tiles);
    auto baked = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(r), r, std::move(*tiles), {kNonce, {}}, report);
    REQUIRE(baked);
    const auto opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(baked->encoded)); REQUIRE(opened);
    auto query = TerrainHeightQuery::Create(*opened.value); REQUIRE(PreloadAllTerrainTiles(query, *opened.value).Ok());
    const auto &fine = TileAt(baked->tiles, {-1, -1});
    const auto position = *TerrainLocalPosition::FromMeters({-1.0, 0.0, -32.0});
    const auto answer = query.HeightAt(position, {baked->info, 1.0}); REQUIRE(answer.HasHeight());
    CHECK(answer.localHeight == fine.At(63, 32));
    const TerrainGenerator generator(r);
    CHECK(answer.localHeight != generator.SampleHeight({-1, -32}));
    // Single source precision adapter remains a pure final-sample copy.
    const auto layout = *r.SourceLayout({-1, -1});
    const auto lookup = [&](TileCoord t) -> std::span<const float> {
        const auto it = std::find_if(baked->tiles.begin(), baked->tiles.end(), [&](const auto &v) { return v.Tile() == t; });
        return it == baked->tiles.end() || it->IntervalsPerTile() != layout.IntervalsPerTile() ? std::span<const float>() : it->Samples();
    };
    CHECK(lookup({-1, -1})[size_t(32) * 65 + 63] == answer.localHeight);
    const auto subLayout = TerrainLayout::Create(1.0, 64, {-61.5, -64.0, 0.0, 0.0}); REQUIRE(subLayout);
    const auto area = BuildTerrainAreaSamples(*subLayout, baked->info, lookup); REQUIRE(area);
    const size_t offset = size_t(-32 - area.value->samples.min.z) * area.value->countX + size_t(-1 - area.value->samples.min.x);
    CHECK(area.value->heights[offset] == answer.localHeight);
    const auto mesh = BuildTerrainTileMesh(*subLayout, baked->info, {-1, -1}, lookup); REQUIRE(mesh.IsOk());
    const auto vertex = std::find_if(mesh.mesh->vertices.begin(), mesh.mesh->vertices.end(), [](const auto &v) {
        return v.position.x == 63.0f && v.position.z == 32.0f;
    }); REQUIRE(vertex != mesh.mesh->vertices.end()); CHECK(vertex->position.y == answer.localHeight);
}

TEST_CASE("Seam algorithm is in v2 identity and prototype settings cannot publish", "[terraintool][seams]")
{
    const auto r = Recipe(SeamJson());
    const auto canonical = CanonicalRecipeText(r);
    CHECK(canonical.find("finalSeams.version=1") != std::string::npos);
    CHECK(canonical.find("finalSeams.weight=smoothstep") != std::string::npos);
    auto old = canonical;
    for (const std::string prefix : {"finalSeams.version=", "finalSeams.bandMultiplier=", "finalSeams.weight=", "derivatives.version=", "derivatives.maxSpacing="})
    {
        const auto pos = old.find(prefix); REQUIRE(pos != std::string::npos); old.erase(pos, old.find('\n', pos) - pos + 1);
    }
    const auto pos = old.find("canonicalVersion=4"); REQUIRE(pos != std::string::npos); old.replace(pos, 18, "canonicalVersion=2");
    CHECK(Fnv1a64(old) != FingerprintRecipe(r).value);
    CHECK(CanonicalRecipeText(LoadExample()).find("finalSeams") == std::string::npos);
    auto tiles = Raw(r); TerrainReport report("prototype", {});
    REQUIRE(AdjustTerrainSeams(r, tiles, report, {4.0, TerrainSeamWeight::Linear}));
    CHECK_FALSE(EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(r), r, std::move(tiles), {kNonce, {}}, report));
    CHECK(report.Status(check::kEncode) == CheckStatus::Failed);
}

TEST_CASE("Missing or tampered seam metadata cannot silently suppress quality warnings", "[terraintool][seams]")
{
    ScratchDir scratch("metadata");
    auto j = SeamJson(); j["parameters"]["detail"]["amplitude"] = 500.0;
    const auto r = Recipe(j);
    TerrainReport report("metadata", {}); auto tiles = BakeFinalHeights(r, report); REQUIRE(tiles);
    Write(scratch.path / "candidate", r, *tiles, report, kNonce);
    const auto text = ReadText(scratch.path / "candidate" / kCandidateManifestFileName); REQUIRE(text);
    Json manifest = Json::parse(*text);
    SECTION("missing summary") { manifest["seams"] = nullptr; }
    SECTION("warning threshold") { manifest["seams"]["warningThreshold"] = 1e9; }
    SECTION("unofficial weight") { manifest["seams"]["weight"] = "linear"; }
    SECTION("unofficial band") { manifest["seams"]["bandMultiplier"] = 4.0; }
    SECTION("invalid position") { manifest["seams"]["adjustments"][0]["position"] = {1e9, 1e9}; }
    SECTION("off-grid position") { manifest["seams"]["adjustments"][0]["position"] = {-0.125, -32.0}; }
    SECTION("zero-weight tile centre") { manifest["seams"]["adjustments"][0]["position"] = {-32.0, -32.0}; }
    SECTION("zero-weight band endpoint")
    {
        auto &a = manifest["seams"]["adjustments"][0];
        const double width = a["bandWidth"].get<double>();
        const std::string border = a["border"].get<std::string>();
        if (border == "left") a["position"][0] = -64.0 + width;
        if (border == "right") a["position"][0] = -width;
        if (border == "bottom") a["position"][1] = -64.0 + width;
        if (border == "top") a["position"][1] = -width;
    }
    SECTION("unbounded band") { manifest["seams"]["adjustments"][0]["bandWidth"] = 1e9; }
    SECTION("non-finite position") { manifest["seams"]["adjustments"][0]["position"] = {nullptr, 0.0}; }
    REQUIRE(WriteText(scratch.path / "candidate" / kCandidateManifestFileName, manifest.dump()));
    const auto rejected = RunTerrainPublish({scratch.path / "candidate", scratch.path / "releases"});
    CHECK_FALSE(rejected.Ok()); CHECK_FALSE(PeekTerrainCurrent(scratch.path / "releases", MakeAssetId(ExampleAssetPath())));
    TerrainReport empty("empty", {});
    CHECK_FALSE(EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(r), r, std::move(*tiles), {kNonce, {}}, empty));
}

TEST_CASE("Sync replaces the previous filtered v2 current after the final seam version changes", "[terraintool][seams]")
{
    ScratchDir scratch("seam_sync");
    const auto j = SeamJson(); const auto r = Recipe(j);
    const auto before = BeforeSeamFingerprint(r); CHECK(before != FingerprintRecipe(r));
    auto tiles = Raw(r);
    const auto layers = r.SourceLayers(); std::vector<TerrainTileWrite> records;
    for (const auto &t : tiles)
    {
        const auto l = std::find_if(layers.begin(), layers.end(), [&](const auto &v) { return v.IntervalsPerTile() == t.IntervalsPerTile(); });
        records.push_back({&t, uint32_t(l - layers.begin()), true, std::nullopt});
    }
    const AssetId asset = MakeAssetId(ExampleAssetPath());
    const auto oldHash = TerrainHeightsHash(layers, records);
    const TerrainReleaseInfo oldInfo{asset, before, kTerrainFormatVersion, MintTerrainReleaseId(asset, before, oldHash, kNonce)};
    const auto encoded = EncodeTerrainRelease(oldInfo, layers, records); REQUIRE(encoded);
    const auto game = scratch.path / "game";
    const auto releases = TerrainReleasesRoot(game);
    const auto dir = TerrainReleaseDir(releases, asset, oldInfo.release); fs::create_directories(dir);
    REQUIRE(WriteBytes(dir / kTerrainIndexFileName, encoded.value->index));
    for (const auto &p : encoded.value->packs) REQUIRE(WriteBytes(dir / p.fileName, p.bytes));
    REQUIRE(WriteText(TerrainCurrentPointerPath(releases, asset), EncodeTerrainCurrentPointer(oldInfo.release)));
    const auto recipePath = scratch.path / "relief-2x2.terrain.json"; REQUIRE(WriteText(recipePath, j.dump()));
    TerrainSyncRequest request{recipePath, std::string(kIdRoot), std::string(kSourcePath), game, scratch.path / "candidate"};
    request.options.nonce = kNonce + 1;
    const auto result = RunTerrainSync(request);
    for (const auto &d : result.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(result.Ok()); REQUIRE(result.publish); CHECK(result.publish->published);
    CHECK(result.publish->previous == oldInfo.release); CHECK(result.publish->current != oldInfo.release);
    CHECK(fs::is_directory(dir));
    const auto again = RunTerrainSync(request); CHECK(again.Ok()); CHECK_FALSE(again.publish->published);
}

TEST_CASE("Mixed seam numerical acceptance also holds on decimal grids around 150 km", "[terraintool][seams]")
{
    for (int32_t shift : {-5860, 5860})
    {
        auto j = SeamJson();
        const double offset = shift * 25.6;
        j["layout"]["sampleSpacing"] = 0.1; j["layout"]["intervalsPerTile"] = 256;
        j["layout"]["bounds"] = {{"minX", offset - 25.125}, {"minZ", -25.6}, {"maxX", offset + 25.6}, {"maxZ", 25.05}};
        j["parameters"]["tilt"]["x"] = 0.0;
        for (auto &t : j["precision"]["tiles"]) { t["x"] = t["x"].get<int32_t>() + shift; t["spacing"] = t["spacing"].get<double>() * 0.1; }
        const auto r = Recipe(j); TerrainReport report("far", {});
        const auto tiles = BakeFinalHeights(r, report); REQUIRE(tiles);
        const auto stats = Check(r, *tiles, report);
        for (const auto &d : report.diagnostics) UNSCOPED_INFO(FormatDiagnostic(d));
        CHECK(report.Ok()); CHECK(stats.maxTolerance < 0.00001); CHECK(stats.maxSurfaceDifference <= stats.maxTolerance);
    }
}
