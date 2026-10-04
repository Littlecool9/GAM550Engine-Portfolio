/**
 * @file TerrainRecipe.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainRecipe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <TerrainRecipe.h>

#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <string>

using namespace mir;
using namespace mir::terrainbake;
using Json = nlohmann::json;

namespace
{
    const std::string kExamplePath = std::string(TERRAIN_FIXTURE_DIR) + "/relief-2x2.terrain.json";

    Json Example()
    {
        std::ifstream in(kExamplePath);
        return Json::parse(in);
    }

    RecipeParseResult Parse(const Json &json)
    {
        return ParseTerrainRecipe(json.dump());
    }

    TerrainRecipe Valid(const Json &json)
    {
        RecipeParseResult result = Parse(json);
        for (const RecipeIssue &issue : result.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(result.recipe);
        return *result.recipe;
    }

    bool HasIssue(const RecipeParseResult &result, std::string_view field, std::string_view text = "")
    {
        for (const RecipeIssue &issue : result.issues)
            if (issue.field == field && issue.message.find(text) != std::string::npos)
                return true;
        return false;
    }

    void Dump(const RecipeParseResult &result)
    {
        for (const RecipeIssue &issue : result.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
    }
}

TEST_CASE("The example recipe loads and validates", "[terraintool][recipe]")
{
    const RecipeParseResult result = LoadTerrainRecipe(kExamplePath);
    Dump(result);
    REQUIRE(result.recipe);
    CHECK(result.issues.empty());

    const TerrainRecipe &recipe = *result.recipe;
    CHECK(recipe.recipeVersion == kTerrainRecipeVersion);
    CHECK(recipe.generatorId == kReliefGeneratorId);
    CHECK(recipe.generatorVersion == kReliefGeneratorVersion);
    CHECK(recipe.seed == 20261001u);
    CHECK(recipe.layout.SampleSpacing() == 4.0);
    CHECK(recipe.layout.IntervalsPerTile() == 64);
    CHECK(recipe.layout.TileSize() == 256.0);
    CHECK(recipe.layout.Tiles().min == TileCoord{-1, -1});
    CHECK(recipe.layout.Tiles().max == TileCoord{0, 0});
    CHECK(recipe.relief.ridges.octaves == 2);
    CHECK(recipe.relief.plainsBlend == 4.0);
}

TEST_CASE("Equal recipes fingerprint alike whatever the JSON spelling", "[terraintool][recipe]")
{
    const TerrainRecipe original = Valid(Example());

    const std::string respelled = R"({
        "parameters": {
            "plains": { "blend": 4, "height": -0.0 },
            "detail": { "octaves": 2, "wavelength": 2.4e1, "amplitude": 1 },
            "ridges": { "octaves": 2, "wavelength": 192, "amplitude": 30.0 },
            "broad": { "octaves": 2, "wavelength": 512.000, "amplitude": 20 },
            "tilt": { "z": -0, "x": 15e-2 },
            "baseHeight": 1.5e1
        },
        "layout": { "bounds": { "maxZ": 250, "maxX": 256, "minZ": -256, "minX": -250 },
                    "intervalsPerTile": 64, "sampleSpacing": 4 },
        "seed": 20261001,
        "generator": { "version": 1, "id": "mir.relief" },
        "recipeVersion": 1 })";
    const RecipeParseResult again = ParseTerrainRecipe(respelled);
    Dump(again);
    REQUIRE(again.recipe);

    CHECK(CanonicalRecipeText(*again.recipe) == CanonicalRecipeText(original));
    CHECK(FingerprintRecipe(*again.recipe) == FingerprintRecipe(original));
    CHECK(FingerprintRecipe(Valid(Example())) == FingerprintRecipe(original));
    CHECK_FALSE(FingerprintRecipe(original).IsNull());
}

TEST_CASE("Changing any declared input changes the fingerprint", "[terraintool][recipe]")
{
    const Json example = Example();
    const TerrainRecipe original = Valid(example);

    const std::pair<const char *, Json> edits[] = {
        {"/seed", 20261002u},
        {"/layout/sampleSpacing", 2.0},
        {"/layout/intervalsPerTile", 128},
        {"/layout/bounds/minX", -251.0},
        {"/layout/bounds/minZ", -255.0},
        {"/layout/bounds/maxX", 255.0},
        {"/layout/bounds/maxZ", 251.0},
        {"/parameters/baseHeight", 0.5},
        {"/parameters/tilt/x", 0.0600001},
        {"/parameters/tilt/z", 0.01},
        {"/parameters/broad/amplitude", 12.5},
        {"/parameters/broad/wavelength", 1000.0},
        {"/parameters/broad/octaves", 3},
        {"/parameters/ridges/amplitude", 21.0},
        {"/parameters/ridges/wavelength", 250.0},
        {"/parameters/ridges/octaves", 3},
        {"/parameters/detail/amplitude", 0.5},
        {"/parameters/detail/wavelength", 30.0},
        {"/parameters/detail/octaves", 1},
        {"/parameters/plains/height", 1.0},
        {"/parameters/plains/blend", 0.0},
    };

    std::set<uint64_t> fingerprints{FingerprintRecipe(original).value};
    for (const auto &[pointer, value] : edits)
    {
        INFO(pointer);
        Json edited = example;
        edited[Json::json_pointer(pointer)] = value;
        CHECK(fingerprints.insert(FingerprintRecipe(Valid(edited)).value).second);
    }

    // Versions cannot be edited in a valid file but still count.
    TerrainRecipe newerGenerator = original;
    newerGenerator.generatorVersion = kReliefGeneratorVersion + 1;
    CHECK(fingerprints.insert(FingerprintRecipe(newerGenerator).value).second);

    TerrainRecipe otherGenerator = original;
    otherGenerator.generatorId = "mir.other";
    CHECK(fingerprints.insert(FingerprintRecipe(otherGenerator).value).second);

    TerrainRecipe newerSchema = original;
    newerSchema.recipeVersion = kTerrainRecipeVersion + 1;
    CHECK(fingerprints.insert(FingerprintRecipe(newerSchema).value).second);
}

TEST_CASE("The example recipe fingerprint is pinned", "[terraintool][recipe]")
{
    // Change only with kReliefGeneratorVersion or kRecipeCanonicalVersion.
    CHECK(FingerprintRecipe(Valid(Example())).value == 0x288219ec4e313cb7ull);
}

TEST_CASE("Every invalid or unknown field is reported by name before any output", "[terraintool][recipe]")
{
    Json json = Example();
    json["comment"] = "not part of the schema";
    json["seed"] = -5;
    json["layout"]["intervalsPerTile"] = 0;
    json["layout"]["bounds"]["maxX"] = -300.0;
    json["parameters"]["tilt"]["x"] = "steep";
    json["parameters"]["broad"]["amplitude"] = -1.0;
    json["parameters"]["ridges"]["sharpness"] = 3;
    json["parameters"]["detail"]["octaves"] = 2.5;
    json["parameters"].erase("plains");

    const RecipeParseResult result = Parse(json);
    Dump(result);
    CHECK_FALSE(result.recipe);
    CHECK(HasIssue(result, "comment", "unknown field"));
    CHECK(HasIssue(result, "comment", "not part of the schema"));
    CHECK(HasIssue(result, "seed", "-5"));
    CHECK(HasIssue(result, "layout.intervalsPerTile", "0 is out of range"));
    CHECK(HasIssue(result, "layout.bounds", "maxX -300"));
    CHECK(HasIssue(result, "parameters.tilt.x", "\"steep\""));
    CHECK(HasIssue(result, "parameters.broad.amplitude", "-1 is out of range [0, 10000] m"));
    CHECK(HasIssue(result, "parameters.ridges.sharpness", "unknown field"));
    CHECK(HasIssue(result, "parameters.detail.octaves", "2.5"));
    CHECK(HasIssue(result, "parameters.plains", "missing"));
    CHECK(result.issues.size() == 9);
}

TEST_CASE("Values just outside a range are refused, never clamped", "[terraintool][recipe]")
{
    Json example = Example();
    // Room for 8 octaves: 1024 m / 2^7 = 8 m = 2 x sampleSpacing.
    example["parameters"]["broad"]["wavelength"] = 1024.0;
    const auto withValue = [&](const char *pointer, const Json &value)
    {
        Json edited = example;
        edited[Json::json_pointer(pointer)] = value;
        return Parse(edited);
    };

    CHECK(withValue("/parameters/tilt/x", 1.0).recipe);
    CHECK(HasIssue(withValue("/parameters/tilt/x", 1.0000000000000002), "parameters.tilt.x", "out of range"));
    CHECK(withValue("/parameters/broad/octaves", 8).recipe);
    CHECK(HasIssue(withValue("/parameters/broad/octaves", 9), "parameters.broad.octaves", "9 is out of range [1, 8]"));
    CHECK(withValue("/parameters/plains/blend", 1000.0).recipe);
    CHECK(HasIssue(withValue("/parameters/plains/blend", 1000.0000000000001), "parameters.plains.blend"));
    CHECK(HasIssue(withValue("/layout/sampleSpacing", 0.0), "layout.sampleSpacing", "greater than 0"));

    CHECK(withValue("/parameters/detail/wavelength", 16.0).recipe);
    CHECK(HasIssue(withValue("/parameters/detail/wavelength", 15.0), "parameters.detail", "finest octave wavelength 7.5 m"));

    // Each value in range, but together they could leave +-10 km.
    CHECK(HasIssue(withValue("/parameters/baseHeight", 9990.0), "parameters", "beyond +-10000 m"));
}

TEST_CASE("Unsupported generators and versions are refused", "[terraintool][recipe]")
{
    Json other = Example();
    other["generator"]["id"] = "mir.erosion";
    other["parameters"]["rivers"] = true;
    const RecipeParseResult otherResult = Parse(other);
    Dump(otherResult);
    CHECK_FALSE(otherResult.recipe);
    CHECK(HasIssue(otherResult, "generator.id", "\"mir.erosion\""));
    CHECK(otherResult.issues.size() == 1);

    Json newer = Example();
    newer["generator"]["version"] = 2;
    CHECK(HasIssue(Parse(newer), "generator.version", "unsupported version 2"));

    Json schema = Example();
    schema["recipeVersion"] = 2;
    CHECK(HasIssue(Parse(schema), "recipeVersion", "unsupported recipe version 2"));
}

TEST_CASE("Malformed documents, non-finite numbers and duplicate keys are reported", "[terraintool][recipe]")
{
    // A syntax error stops the parse; the issue names the field being read.
    CHECK(HasIssue(ParseTerrainRecipe("{ \"seed\": "), "seed", "not valid JSON"));
    CHECK(HasIssue(ParseTerrainRecipe("[1, 2]"), "", "expected an object"));

    std::string text = Example().dump();
    // Keys dump sorted, so this is parameters.broad's.
    const std::string amplitude = "\"amplitude\":20.0";
    REQUIRE(text.find(amplitude) != std::string::npos);
    std::string huge = text;
    huge.replace(huge.find(amplitude), amplitude.size(), "\"amplitude\":1e400");
    CHECK(HasIssue(ParseTerrainRecipe(huge), "parameters.broad.amplitude", "number overflow"));

    std::string repeated = text;
    repeated.insert(1, "\"seed\":7,");
    const RecipeParseResult duplicate = ParseTerrainRecipe(repeated);
    CHECK_FALSE(duplicate.recipe);
    CHECK(HasIssue(duplicate, "seed", "duplicate"));

    // Each field is fine, but the grid cannot resolve samples this close at this reach.
    Json fine = Example();
    fine["layout"]["sampleSpacing"] = 1.0e-9;
    CHECK(HasIssue(Parse(fine), "layout", "TerrainLayout::Create"));

    // Past this a tile block would exceed TileHeights' limit.
    Json wide = Example();
    wide["layout"]["intervalsPerTile"] = kMaxTerrainTileIntervals + 1;
    CHECK(HasIssue(Parse(wide), "layout.intervalsPerTile", "out of range [1, 4096]"));

    // 511 tiles of 64 intervals at 1 m are 32705 samples; one more tile is 32769.
    Json large = Example();
    large["parameters"]["tilt"] = {{"x", 0.0}, {"z", 0.0}};
    large["layout"]["sampleSpacing"] = 1.0;
    large["layout"]["intervalsPerTile"] = 64;
    large["layout"]["bounds"] = {{"minX", 0.0}, {"minZ", 0.0}, {"maxX", 32704.0}, {"maxZ", 64.0}};
    Valid(large);
    large["layout"]["bounds"]["maxX"] = 32704.5;
    CHECK(HasIssue(Parse(large), "layout.bounds", "32769 x 65 tile samples exceed 32768"));

    CHECK(HasIssue(LoadTerrainRecipe(std::string(TERRAIN_FIXTURE_DIR) + "/missing.terrain.json"), "", "cannot read"));
}
