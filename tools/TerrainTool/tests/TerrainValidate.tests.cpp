/**
 * @file TerrainValidate.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainValidate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainContinuity.h>
#include <TerrainValidate.h>

#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    TerrainReport Validate(const fs::path &dir, const TerrainRecipe *recipe = nullptr, bool requireManifest = true)
    {
        TerrainReport report("validate", {check::kCandidate, check::kFormat, check::kIndex, check::kPacks,
                                          check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                                          check::kCorners, check::kIdentity});
        ValidateTerrainCandidate(dir, {recipe, requireManifest}, report);
        return report;
    }

    std::vector<std::string> FailedChecks(const TerrainReport &report)
    {
        std::vector<std::string> failed;
        for (const TerrainCheck &c : report.checks)
            if (c.status == CheckStatus::Failed)
                failed.push_back(c.name);
        return failed;
    }

    using Names = std::vector<std::string>;
}

TEST_CASE("A baked candidate passes every check, with and without its recipe", "[terraintool][validate]")
{
    ScratchDir scratch("validate_ok");
    const fs::path dir = scratch.path / "candidate";
    const TerrainBakedRelease baked = WriteCraftedCandidate(dir, [](auto &) {});
    const TerrainRecipe recipe = LoadExample();

    const TerrainReport report = Validate(dir, &recipe);
    CHECK(FailedChecks(report).empty());
    for (const TerrainCheck &c : report.checks)
    {
        INFO(c.name << ": " << c.detail);
        CHECK(c.status == CheckStatus::Passed);
    }
    REQUIRE(report.release);
    CHECK(*report.release == baked.info);
    CHECK(report.heightsHash == baked.encoded.heightsHash);
    CHECK(report.surfaceVersion == kTerrainSurfaceVersion);
    CHECK(report.tiles.size() == 4);

    CHECK(Validate(dir).Status(check::kIdentity) == CheckStatus::Passed);

    SECTION("a release directory without candidate.json, when the caller allows it")
    {
        fs::remove(dir / kCandidateManifestFileName);
        CHECK(FailedChecks(Validate(dir, &recipe, false)).empty());
        const TerrainReport bare = Validate(dir, nullptr, false);
        CHECK(FailedChecks(bare).empty());
        CHECK(bare.Status(check::kIdentity) == CheckStatus::Skipped);
        const std::vector<TerrainDiagnostic> required = DiagnosticsOf(Validate(dir), check::kCandidate);
        REQUIRE(required.size() == 1);
        CHECK(required[0].field == "candidate.json");
    }
}

TEST_CASE("Mismatched shared edge samples fail the edge check with both tiles named", "[terraintool][validate]")
{
    ScratchDir scratch("validate_edge");
    const fs::path dir = scratch.path / "candidate";
    // Tile-local (0, 10) of (0, -1) is global sample (0, -54).
    WriteCraftedCandidate(dir, [](std::vector<TileHeights> &tiles) {
        TileHeights &tile = TileAt(tiles, {0, -1});
        tile.Set(0, 10, tile.At(0, 10) + 0.5f);
    });

    const TerrainReport report = Validate(dir, nullptr);
    CHECK(FailedChecks(report) == Names{"edges"});
    const std::vector<TerrainDiagnostic> edges = DiagnosticsOf(report, check::kEdges);
    REQUIRE(edges.size() == 2);
    CHECK(edges[0].field == "samples");
    CHECK(SameTiles(edges[0].tiles, {{-1, -1}, {0, -1}}));
    REQUIRE(edges[0].sample);
    CHECK(*edges[0].sample == GlobalSampleIndex{0, -54});
    CHECK(edges[0].message.find("1 of 65") != std::string::npos);
    CHECK(edges[1].field == "surface");
    CHECK(SameTiles(edges[1].tiles, {{-1, -1}, {0, -1}}));
    CHECK(report.Status(check::kCorners) == CheckStatus::Passed);
}

TEST_CASE("A mismatched four-tile corner fails the corner check with all four tiles named", "[terraintool][validate]")
{
    ScratchDir scratch("validate_corner");
    const fs::path dir = scratch.path / "candidate";
    WriteCraftedCandidate(dir, [](std::vector<TileHeights> &tiles) {
        TileHeights &tile = TileAt(tiles, {0, 0});
        tile.Set(0, 0, tile.At(0, 0) + 0.25f);
    });

    const TerrainReport report = Validate(dir);
    CHECK(FailedChecks(report) == Names{"edges", "corners"});
    const std::vector<TerrainDiagnostic> corners = DiagnosticsOf(report, check::kCorners);
    REQUIRE(corners.size() == 2);
    for (const TerrainDiagnostic &d : corners)
    {
        CHECK(SameTiles(d.tiles, {{-1, -1}, {0, -1}, {-1, 0}, {0, 0}}));
        REQUIRE(d.sample);
        CHECK(*d.sample == GlobalSampleIndex{0, 0});
    }
    CHECK(corners[0].field == "samples");
    CHECK(corners[1].field == "surface");
    std::vector<std::vector<TileCoord>> edgeTiles;
    for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kEdges))
        if (d.field == "samples")
            edgeTiles.push_back(d.tiles);
    CHECK(edgeTiles == std::vector<std::vector<TileCoord>>{{{0, -1}, {0, 0}}, {{-1, 0}, {0, 0}}});
}

TEST_CASE("Shared samples must match bit for bit, not only in value", "[terraintool][validate]")
{
    ScratchDir scratch("validate_bits");
    const fs::path dir = scratch.path / "candidate";
    // The western plains are exactly +0 m; -0 equals +0 but is another stored value.
    bool flipped = false;
    WriteCraftedCandidate(dir, [&](std::vector<TileHeights> &tiles) {
        TileHeights &below = TileAt(tiles, {-1, -1});
        TileHeights &above = TileAt(tiles, {-1, 0});
        for (int32_t i = 2; i < 64 && !flipped; ++i)
            if (std::bit_cast<uint32_t>(above.At(i, 0)) == 0u && std::bit_cast<uint32_t>(below.At(i, 64)) == 0u)
            {
                above.Set(i, 0, -0.0f);
                flipped = true;
            }
    });
    REQUIRE(flipped);

    const TerrainReport report = Validate(dir);
    const std::vector<TerrainDiagnostic> edges = DiagnosticsOf(report, check::kEdges);
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].field == "samples");
    CHECK(SameTiles(edges[0].tiles, {{-1, -1}, {-1, 0}}));
}

TEST_CASE("Damaged release files fail with the tile and file named", "[terraintool][validate]")
{
    ScratchDir scratch("validate_damage");
    const fs::path dir = scratch.path / "candidate";
    WriteCraftedCandidate(dir, [](auto &) {});

    SECTION("a missing pack is a missing tile")
    {
        const TileLocation where = Locate(dir, {0, 0});
        fs::remove(dir / where.pack);
        const TerrainReport report = Validate(dir);
        CHECK(FailedChecks(report) == Names{"packs", "coverage"});
        const std::vector<TerrainDiagnostic> coverage = DiagnosticsOf(report, check::kCoverage);
        REQUIRE(coverage.size() == 1);
        CHECK(coverage[0].field == "chunk_0_0.tpack");
        CHECK(SameTiles(coverage[0].tiles, {{0, 0}}));
        // Seams into the missing tile cannot be compared, nor its samples read, so they are not passing.
        CHECK(report.Status(check::kEdges) == CheckStatus::Skipped);
        CHECK(report.Status(check::kCorners) == CheckStatus::Skipped);
        CHECK(report.Status(check::kPayloads) == CheckStatus::Skipped);
        CHECK(report.Status(check::kFinite) == CheckStatus::Skipped);
        CHECK(report.tiles.size() == 4);
    }
    SECTION("a corrupt payload byte")
    {
        const TileLocation where = Locate(dir, {-1, 0});
        std::vector<std::byte> pack = ReadFile(dir / where.pack);
        pack[size_t(where.offset) + 100] ^= std::byte{0x01};
        WriteFile(dir / where.pack, pack);
        const TerrainReport report = Validate(dir);
        CHECK(FailedChecks(report) == Names{"packs", "payloads"});
        const std::vector<TerrainDiagnostic> payloads = DiagnosticsOf(report, check::kPayloads);
        REQUIRE(payloads.size() == 1);
        CHECK(payloads[0].field == "payload");
        CHECK(SameTiles(payloads[0].tiles, {{-1, 0}}));
        CHECK(payloads[0].message.find("checksum") != std::string::npos);
        // The corrupt tile's heights were never decoded.
        CHECK(report.Status(check::kFinite) == CheckStatus::Skipped);
    }
    SECTION("a resealed non-finite height")
    {
        PatchSampleSealed(dir, {0, -1}, 5, 7, std::numeric_limits<float>::quiet_NaN());
        const TerrainReport report = Validate(dir);
        const std::vector<TerrainDiagnostic> finite = DiagnosticsOf(report, check::kFinite);
        REQUIRE(finite.size() == 1);
        CHECK(finite[0].field == "samples");
        CHECK(SameTiles(finite[0].tiles, {{0, -1}}));
        REQUIRE(finite[0].sample);
        CHECK(*finite[0].sample == GlobalSampleIndex{5, -57});
        CHECK(report.Status(check::kPayloads) == CheckStatus::Passed);
        CHECK(report.Status(check::kIdentity) == CheckStatus::Failed);
    }
    SECTION("an unsupported major format")
    {
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        StoreU32(index, 4, 3);
        WriteFile(dir / kTerrainIndexFileName, index);
        const TerrainReport report = Validate(dir);
        CHECK(FailedChecks(report) == Names{"format"});
        CHECK(report.Status(check::kEdges) == CheckStatus::Skipped);
        CHECK_FALSE(report.release);
    }
    SECTION("a stray file and a broken manifest")
    {
        WriteFile(dir / "chunk_9_9.tpack", std::vector<std::byte>(8));
        REQUIRE(WriteText(dir / kCandidateManifestFileName, "{\"kind\": \"mir.terrain.candidate\"}"));
        const TerrainReport report = Validate(dir);
        CHECK(FailedChecks(report) == Names{"candidate"});
        Names fields;
        for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kCandidate))
            fields.push_back(d.field);
        CHECK(std::find(fields.begin(), fields.end(), "chunk_9_9.tpack") != fields.end());
        CHECK(std::find(fields.begin(), fields.end(), "release") != fields.end());
    }
}

TEST_CASE("Validating against a recipe checks the source fingerprint and layout", "[terraintool][validate]")
{
    ScratchDir scratch("validate_recipe");
    const fs::path dir = scratch.path / "candidate";
    WriteCraftedCandidate(dir, [](auto &) {});

    std::ifstream in(ExampleRecipePath());
    Json json = Json::parse(in);
    json["seed"] = 7;
    json["layout"]["bounds"]["maxX"] = 200.0;
    const RecipeParseResult other = ParseTerrainRecipe(json.dump());
    REQUIRE(other.recipe);

    const TerrainReport report = Validate(dir, &*other.recipe);
    CHECK(FailedChecks(report) == Names{"identity"});
    Names fields;
    for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kIdentity))
        fields.push_back(d.field);
    CHECK(fields == Names{"sourceFingerprint", "layout.bounds.maxX"});
}

TEST_CASE("Continuity counts what it could not compare", "[terraintool][validate]")
{
    const TerrainRecipe recipe = LoadExample();
    TerrainReport scratch("craft", {});
    std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, scratch);
    REQUIRE(tiles);
    std::unordered_map<TileCoord, const TileHeights *> byCoord;
    for (const TileHeights &tile : *tiles)
        byCoord[tile.Tile()] = &tile;

    TileSampleLookup lookup = [&](TileCoord tile) -> std::span<const float> {
        const auto it = byCoord.find(tile);
        return it == byCoord.end() ? std::span<const float>() : it->second->Samples();
    };
    TerrainReport all("validate", {check::kEdges, check::kCorners});
    const ContinuityStats full = CheckTileContinuity(recipe.layout, lookup, all);
    CHECK(full.edges == 4);
    CHECK(full.edgesCompared == 4);
    CHECK(full.corners == 1);
    CHECK(full.cornersCompared == 1);
    // 257 probes per 256 m edge, minus 6 past the bounds on each clipped edge, plus the corner.
    CHECK(full.probes == 257 + 251 + 257 + 251 + 1);
    CHECK(full.maxSurfaceDifference == 0.0);
    CHECK(all.Ok());
    CHECK(all.Status(check::kEdges) == CheckStatus::Passed);

    byCoord.erase(TileCoord{0, 0});
    TerrainReport partial("validate", {check::kEdges, check::kCorners});
    const ContinuityStats some = CheckTileContinuity(recipe.layout, lookup, partial);
    CHECK(some.edgesCompared == 2);
    CHECK(some.cornersCompared == 0);
    CHECK(partial.Status(check::kEdges) == CheckStatus::Skipped);
    CHECK(partial.Status(check::kCorners) == CheckStatus::Skipped);
}
