#include "TerrainBakeTestSupport.h"

#include <TerrainGenerator.h>
#include <TerrainValidate.h>
#include <terrain/TerrainHeightQuery.h>

#include <bit>
#include <cmath>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    Json MixedJson()
    {
        const auto text = ReadText(fs::path(TERRAIN_FIXTURE_DIR) / "mixed-2x2.terrain.json");
        REQUIRE(text);
        return Json::parse(*text);
    }

    TerrainRecipe Mixed(const Json &json = MixedJson())
    {
        const auto parsed = ParseTerrainRecipe(json.dump());
        for (const auto &issue : parsed.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(parsed.recipe);
        return *parsed.recipe;
    }

    bool Issue(const RecipeParseResult &parsed, std::string_view field, std::string_view text)
    {
        return std::any_of(parsed.issues.begin(), parsed.issues.end(), [&](const auto &issue) {
            return issue.field == field && issue.message.find(text) != std::string::npos;
        });
    }

    TerrainBakedRelease BakeMixed(const TerrainRecipe &recipe)
    {
        TerrainReport report("mixed", {});
        auto tiles = BakeFinalHeights(recipe, report);
        REQUIRE(tiles);
        auto baked = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe,
                                       std::move(*tiles), {kNonce, {}}, report);
        REQUIRE(baked);
        REQUIRE(report.Ok());
        return std::move(*baked);
    }

    double PyramidPoint(const TerrainGenerator &generator, const TerrainLayout &reference,
                        int level, int64_t x, int64_t z)
    {
        if (level == 0)
            return generator.Height(reference.SamplePosition(x), reference.SamplePosition(z));
        double rows[3];
        for (int k = -1; k <= 1; ++k)
            rows[k + 1] = (PyramidPoint(generator, reference, level - 1, x * 2 - 1, z * 2 + k) +
                2.0 * PyramidPoint(generator, reference, level - 1, x * 2, z * 2 + k) +
                PyramidPoint(generator, reference, level - 1, x * 2 + 1, z * 2 + k)) * 0.25;
        return (rows[0] + 2.0 * rows[1] + rows[2]) * 0.25;
    }
}

TEST_CASE("Mixed source rules have exactly one assigned spacing per signed tile", "[terraintool][precision]")
{
    const auto recipe = Mixed();
    CHECK(recipe.layout.SampleSpacing() == 1.0);
    CHECK(recipe.layout.TileSize() == 256.0);
    CHECK(recipe.SourceSpacing({-1, -1}) == 1.0);
    CHECK(recipe.SourceSpacing({0, -1}) == 8.0);
    CHECK(recipe.SourceSpacing({-1, 0}) == 2.0);
    CHECK(recipe.SourceSpacing({0, 0}) == 4.0);
    CHECK_FALSE(recipe.SourceSpacing({1, 0}));
    CHECK(recipe.sourceRules.size() == 4);
    const auto layers = recipe.SourceLayers();
    REQUIRE(layers.size() == 4);
    for (size_t i = 0; i < layers.size(); ++i)
    {
        CHECK(layers[i].SampleSpacing() == double(1u << i));
        CHECK(layers[i].TileSize() == recipe.layout.TileSize());
        CHECK(layers[i].Bounds() == recipe.layout.Bounds());
        if (i > 0)
            CHECK(TerrainLayersNest(layers[i - 1], layers[i]));
    }
}

TEST_CASE("Explicit source defaults fill only unassigned tiles", "[terraintool][precision]")
{
    Json json = MixedJson();
    json["precision"]["defaultSpacing"] = 8.0;
    json["precision"]["tiles"] = Json::array({{{"x", -1}, {"z", -1}, {"spacing", 1.0}}});
    const auto recipe = Mixed(json);
    CHECK(recipe.SourceSpacing({-1, -1}) == 1.0);
    CHECK(recipe.SourceSpacing({0, -1}) == 8.0);
    CHECK(recipe.SourceSpacing({-1, 0}) == 8.0);
    CHECK(recipe.SourceSpacing({0, 0}) == 8.0);
    const auto baked = BakeMixed(recipe);
    const auto index = ParseTerrainIndex(baked.encoded.index);
    REQUIRE(index);
    CHECK(index.value->tiles.size() == 4);
    CHECK(index.value->layers.size() == 2);
    CHECK(index.value->Find({0, -1})->source);
    CHECK(index.value->Find({0, -1})->sampleSpacing == 8.0);
}

TEST_CASE("Missing duplicate outside and non-nested source rules are rejected", "[terraintool][precision]")
{
    Json json = MixedJson();
    SECTION("coverage")
    {
        json["precision"]["tiles"].erase(0);
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles", "missing source assignments"));
    }
    SECTION("identical duplicate")
    {
        json["precision"]["tiles"].push_back(json["precision"]["tiles"][0]);
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles[4]", "duplicate tile"));
    }
    SECTION("conflicting duplicate cannot be prioritized")
    {
        json["precision"]["tiles"].push_back({{"x", -1}, {"z", -1}, {"spacing", 8.0}});
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles[4]", "overlapping"));
    }
    SECTION("outside")
    {
        json["precision"]["tiles"][0]["x"] = -2;
        const auto parsed = ParseTerrainRecipe(json.dump());
        CHECK(Issue(parsed, "precision.tiles[0]", "outside"));
        CHECK(Issue(parsed, "precision.tiles", "missing"));
    }
    SECTION("spacing")
    {
        for (double spacing : {0.0, -1.0, 0.5, 1.5, 3.0, 512.0})
        {
            INFO(spacing);
            json["precision"]["tiles"][0]["spacing"] = spacing;
            CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles[0].spacing", "power-of-two"));
        }
    }
    SECTION("default")
    {
        json["precision"]["defaultSpacing"] = 3.0;
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.defaultSpacing", "power-of-two"));
    }
    SECTION("explicit default field required")
    {
        json["precision"].erase("defaultSpacing");
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.defaultSpacing", "missing"));
    }
    SECTION("array and integer coordinates")
    {
        json["precision"]["tiles"][0]["x"] = -1.5;
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles[0].x", "integer"));
        json["precision"]["tiles"] = 1;
        CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.tiles", "array"));
    }
}

TEST_CASE("Precision canonical text is order independent and versions separate filtering", "[terraintool][precision]")
{
    const auto original = Mixed();
    Json reversed = MixedJson();
    std::reverse(reversed["precision"]["tiles"].begin(), reversed["precision"]["tiles"].end());
    CHECK(FingerprintRecipe(Mixed(reversed)) == FingerprintRecipe(original));
    reversed["precision"]["tiles"][0]["spacing"] = 8.0;
    CHECK(FingerprintRecipe(Mixed(reversed)) != FingerprintRecipe(original));
    Json wrong = MixedJson();
    wrong["generator"]["version"] = 1;
    CHECK(Issue(ParseTerrainRecipe(wrong.dump()), "generator.version", "unsupported"));
    wrong = MixedJson();
    wrong["recipeVersion"] = 1;
    wrong["generator"]["version"] = 1;
    CHECK(Issue(ParseTerrainRecipe(wrong.dump()), "precision", "requires recipeVersion 2"));
    CHECK(FingerprintRecipe(LoadExample()).value == 0x288219ec4e313cb7ull);
}

TEST_CASE("Height filtering is a double full-height pyramid with global halo", "[terraintool][precision]")
{
    const auto recipe = Mixed();
    const TerrainGenerator generator(recipe);
    bool differsFromRaw = false;
    for (uint32_t level = 1; level <= 3; ++level)
    {
        const auto layout = recipe.SourceLayers()[level];
        for (const GlobalSampleIndex point : {GlobalSampleIndex{-7, -3}, {0, 0}, {4, 9}, {-1, 1}})
        {
            const float expected = float(PyramidPoint(generator, recipe.layout, int(level), point.x, point.z));
            const float filtered = generator.SampleHeight(layout, point);
            CHECK(std::bit_cast<uint32_t>(filtered) == std::bit_cast<uint32_t>(expected));
            const float raw = float(generator.Height(layout.SamplePosition(point.x), layout.SamplePosition(point.z)));
            differsFromRaw |= std::bit_cast<uint32_t>(raw) != std::bit_cast<uint32_t>(filtered);
        }
    }
    CHECK(differsFromRaw);
}

TEST_CASE("Filtered planes preserve height without octave renormalization", "[terraintool][precision]")
{
    Json json = MixedJson();
    json["parameters"]["baseHeight"] = 128.0;
    json["parameters"]["tilt"] = {{"x", 0.125}, {"z", -0.25}};
    for (auto term : {"broad", "ridges", "detail"})
        json["parameters"][term]["amplitude"] = 0.0;
    json["parameters"]["plains"] = {{"height", -1000.0}, {"blend", 0.0}};
    const auto recipe = Mixed(json);
    const TerrainGenerator generator(recipe);
    for (const auto &layout : recipe.SourceLayers())
        for (const GlobalSampleIndex point : {GlobalSampleIndex{-3, -2}, {0, 0}, {5, 6}})
            CHECK(generator.SampleHeight(layout, point) == float(generator.Height(layout.SamplePosition(point.x),
                                                                                  layout.SamplePosition(point.z))));
}

TEST_CASE("A local precision edit never changes other generated source tiles", "[terraintool][precision]")
{
    const auto original = Mixed();
    Json changed = MixedJson();
    changed["precision"]["tiles"][0]["spacing"] = 8.0;
    const auto edited = Mixed(changed);
    CHECK(edited.layout.SampleSpacing() == original.layout.SampleSpacing());
    CHECK(edited.SourceSpacing({0, -1}) == original.SourceSpacing({0, -1}));
    const TerrainGenerator before(original), after(edited);
    for (const auto &rule : original.sourceRules)
    {
        if (rule.tile == TileCoord{-1, -1})
            continue;
        const auto layoutBefore = *original.SourceLayout(rule.tile);
        const auto layoutAfter = *edited.SourceLayout(rule.tile);
        REQUIRE(layoutBefore.SampleSpacing() == layoutAfter.SampleSpacing());
        std::vector<float> a(TileSampleCount(layoutBefore)), b(TileSampleCount(layoutAfter));
        REQUIRE(before.FillTile(layoutBefore, rule.tile, a).Ok());
        REQUIRE(after.FillTile(layoutAfter, rule.tile, b).Ok());
        CHECK(a == b);
    }
    CHECK(original.SourceLayout({-1, -1})->IntervalsPerTile() !=
          edited.SourceLayout({-1, -1})->IntervalsPerTile());
}

TEST_CASE("Mixed source grids retain index precision and independent range reads", "[terraintool][precision]")
{
    const auto recipe = Mixed();
    const auto baked = BakeMixed(recipe);
    auto memory = MakeTerrainMemorySource(baked.encoded);
    auto opened = TerrainReleaseHandle::Open(memory);
    REQUIRE(opened);
    const auto &handle = *opened.value;
    REQUIRE(handle.Index().tiles.size() == 4);
    for (const auto &record : handle.Index().tiles)
    {
        CHECK(record.source);
        CHECK(record.sampleSpacing == recipe.SourceSpacing(record.tile));
        CHECK(record.samplesPerSide == int32_t(recipe.layout.TileSize() / record.sampleSpacing) + 1);
        CHECK(record.length == uint64_t(record.samplesPerSide) * record.samplesPerSide * sizeof(float));
        memory->ClearRequests();
        const auto read = handle.ReadTileRecord(record);
        REQUIRE(read);
        CHECK(read.value->SamplesPerSide() == record.samplesPerSide);
        const auto requests = memory->Requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].offset == record.offset);
        CHECK(requests[0].length == record.length);
    }
    auto query = TerrainHeightQuery::Create(handle);
    REQUIRE(PreloadAllTerrainTiles(query, handle).Ok());
    const size_t readsBefore = memory->Requests().size();
    for (const auto &rule : recipe.sourceRules)
    {
        const double x = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(rule.tile.x)) + 127.25;
        const double z = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(rule.tile.z)) + 123.75;
        const auto answer = query.HeightAt(*TerrainLocalPosition::FromMeters({x, 0.0, z}), {baked.info, 1.0});
        REQUIRE(answer.HasHeight());
        CHECK(answer.sourceSpacing == rule.spacing);
        CHECK(answer.spacing == rule.spacing);
        CHECK(answer.status == (rule.spacing == 1.0 ? TerrainHeightStatus::MeetsRequirement :
                                                    TerrainHeightStatus::CoarserAtSourceLimit));
    }
    if constexpr (mir::kLargeWorld)
    {
        for (double offset : {-150000.25, 150000.5})
        {
            const TerrainPlacement placement{baked.info.asset, *WorldPosition::FromDoubleMeters({offset, -20.0, -offset})};
            const auto local = *TerrainLocalPosition::FromMeters({12.25, 0.0, -127.75});
            const auto world = *TryTerrainLocalToWorld(local, placement.translation);
            const auto near = query.HeightAt(local, {baked.info, 1.0});
            const auto far = query.HeightAt(world, placement, {baked.info, 1.0});
            CHECK(far.status == near.status);
            CHECK(far.sourceSpacing == near.sourceSpacing);
            CHECK(far.localHeight == near.localHeight);
        }
    }
    CHECK(memory->Requests().size() == readsBefore);
}

TEST_CASE("Filtered source fill is independent of tile order and range grouping", "[terraintool][precision]")
{
    const auto recipe = Mixed();
    const TerrainGenerator generator(recipe);
    const auto layout = recipe.SourceLayers()[3];
    std::vector<float> a(TileSampleCount(layout)), b(a.size());
    REQUIRE(generator.FillTile(layout, {-1, -1}, a).Ok());
    REQUIRE(generator.FillTile(layout, {0, -1}, b).Ok());
    const int n = layout.IntervalsPerTile(), side = n + 1;
    for (int z = 0; z <= n; ++z)
        CHECK(std::bit_cast<uint32_t>(a[size_t(z) * side + n]) == std::bit_cast<uint32_t>(b[size_t(z) * side]));
    const auto range = *layout.TileSamples({-1, -1});
    for (const GlobalSampleIndex p : {range.min, range.max, GlobalSampleIndex{-1, -1}})
    {
        const auto at = size_t(p.z - range.min.z) * side + size_t(p.x - range.min.x);
        CHECK(std::bit_cast<uint32_t>(a[at]) == std::bit_cast<uint32_t>(generator.SampleHeight(layout, p)));
    }
    std::reverse(a.begin(), a.end());
    REQUIRE(generator.FillTile(layout, {-1, -1}, a).Ok());
    CHECK(a.back() == generator.SampleHeight(layout, range.max));
    TerrainReport report("order", {});
    auto tiles = BakeFinalHeights(recipe, report);
    REQUIRE(tiles);
    const auto forward = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe,
                                            *tiles, {kNonce, {}}, report);
    std::reverse(tiles->begin(), tiles->end());
    const auto backward = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe,
                                             *tiles, {kNonce, {}}, report);
    REQUIRE(forward);
    REQUIRE(backward);
    CHECK(forward->encoded.index == backward->encoded.index);
    REQUIRE(forward->encoded.packs.size() == backward->encoded.packs.size());
    for (size_t p = 0; p < forward->encoded.packs.size(); ++p)
        CHECK(forward->encoded.packs[p].bytes == backward->encoded.packs[p].bytes);
}

TEST_CASE("Source sample estimates follow tile size and do not bake global reference samples", "[terraintool][precision]")
{
    const auto recipe = Mixed();
    const auto estimates = recipe.SourceEstimates();
    REQUIRE(estimates.size() == 4);
    uint64_t heightBytes = 0;
    for (const auto &estimate : estimates)
    {
        CHECK(estimate.tileCount == 1);
        const uint64_t side = uint64_t(recipe.layout.TileSize() / estimate.spacing) + 1;
        CHECK(estimate.samplesPerTile == side * side);
        heightBytes += estimate.heightBytes;
    }
    CHECK(heightBytes == 352016);
    Json small = MixedJson();
    small["layout"]["intervalsPerTile"] = 128;
    small["layout"]["bounds"] = {{"minX", -128.0}, {"minZ", -128.0}, {"maxX", 128.0}, {"maxZ", 128.0}};
    CHECK(Mixed(small).layout.TileSize() == 128.0);
    Json allCoarse = MixedJson();
    allCoarse["precision"] = {{"defaultSpacing", 8.0}, {"tiles", Json::array()}};
    const auto coarse = Mixed(allCoarse);
    const auto baked = BakeMixed(coarse);
    const auto index = ParseTerrainIndex(baked.encoded.index);
    REQUIRE(index);
    CHECK(index.value->layers.size() == 2);
    for (const auto &record : index.value->tiles)
        CHECK(record.layer == 1);
    const TerrainGenerator generator(coarse);
    const auto layout = *coarse.SourceLayout({0, 0});
    std::vector<float> samples(TileSampleCount(layout));
    const auto filled = generator.FillTile(layout, {0, 0}, samples);
    REQUIRE(filled.Ok());
    CHECK(filled.peakDoubleSamples < 150000);
    CHECK(samples.size() == 1089);
}

TEST_CASE("Mixed candidate validation reports source assignment and continuous final seams", "[terraintool][precision]")
{
    ScratchDir scratch("mixed_candidate");
    const auto recipePath = scratch.path / "relief-2x2.terrain.json";
    REQUIRE(WriteText(recipePath, MixedJson().dump(2)));
    auto request = ExampleRequest(scratch.path / "candidate");
    request.recipePath = recipePath;
    const auto report = RunTerrainBake(request);
    for (const auto &d : report.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(report.Ok());
    CHECK(report.Status(check::kEdges) == CheckStatus::Passed);
    CHECK(report.Status(check::kCorners) == CheckStatus::Passed);
    REQUIRE(report.tiles.size() == 4);
    for (const auto &summary : report.tiles)
    {
        CHECK(summary.source);
        CHECK(summary.sourceSpacing == Mixed().SourceSpacing(summary.tile));
        CHECK(summary.payloadSpacing == summary.sourceSpacing);
    }
    const auto json = Json::parse(TerrainReportJson(report));
    CHECK(json["sourceEstimates"].size() == 4);
    CHECK(json["filter"]["peakDoubleBytes"].get<uint64_t>() > 0);
    const auto validated = RunTerrainValidate({request.candidateDir, recipePath});
    CHECK(validated.Ok());
    CHECK(validated.Status(check::kIdentity) == CheckStatus::Passed);
}

TEST_CASE("Precision parsing refuses unusable coarse coordinates and estimates decimal grids exactly", "[terraintool][precision]")
{
    Json json = MixedJson();
    json["layout"]["intervalsPerTile"] = 4;
    json["layout"]["bounds"] = {{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 8589934574.0}, {"maxZ", 4.0}};
    json["precision"] = {{"defaultSpacing", 4.0}, {"tiles", Json::array()}};
    for (auto term : {"broad", "ridges", "detail"})
        json["parameters"][term]["amplitude"] = 0.0;
    json["parameters"]["baseHeight"] = 0.0;
    json["parameters"]["tilt"] = {{"x", 0.0}, {"z", 0.0}};
    json["parameters"]["plains"] = {{"height", 0.0}, {"blend", 0.0}};
    CHECK(Issue(ParseTerrainRecipe(json.dump()), "precision.defaultSpacing", "source grid cannot preserve"));
    json["layout"]["sampleSpacing"] = 0.1;
    json["layout"]["intervalsPerTile"] = 344;
    json["layout"]["bounds"] = {{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 34.4}, {"maxZ", 34.4}};
    json["precision"]["defaultSpacing"] = 0.8;
    const auto decimal = Mixed(json);
    const auto estimates = decimal.SourceEstimates();
    REQUIRE(estimates.size() == 1);
    REQUIRE(decimal.SourceLayout({0, 0}));
    CHECK(decimal.SourceLayout({0, 0})->IntervalsPerTile() == 43);
    CHECK(estimates[0].samplesPerTile == 44 * 44);
    json["recipeVersion"] = 1;
    json["generator"]["version"] = 1;
    json.erase("precision");
    json["layout"]["intervalsPerTile"] = 43;
    json["layout"]["bounds"] = {{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 4.3}, {"maxZ", 4.3}};
    CHECK(Mixed(json).SourceEstimates()[0].samplesPerTile == 44 * 44);
}
