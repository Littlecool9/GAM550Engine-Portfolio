/**
 * @file TerrainRegions.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainRegions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainFinalSurface.h>
#include <TerrainPublish.h>
#include <TerrainRegions.h>
#include <TerrainSync.h>
#include <TerrainValidate.h>

#include <terrain/TerrainLod.h>
#include <terrain/TerrainRegion.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <random>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    // Every source spacing on small tiles, with clipped map edges on both sides of the origin.
    TerrainRecipe MixedRecipe()
    {
        TerrainRecipe recipe = LoadExample();
        recipe.recipeVersion = 2;
        recipe.generatorVersion = 2;
        recipe.layout = *TerrainLayout::Create(1, 16, {-30.5, -32, 32, 25.25});
        recipe.defaultSpacing = 8;
        recipe.sourceRules = {{{-1, -1}, 1}, {{0, -1}, 2}, {{-1, 0}, 4}, {{1, 1}, 1}, {{-2, 1}, 2}, {{0, -2}, 4}};
        recipe.relief.ridges = {3, 24, 1};
        recipe.relief.detail = {1, 4, 1};
        recipe.relief.plainsHeight = -1000;
        return recipe;
    }

    std::vector<TileHeights> FinalHeights(const TerrainRecipe &recipe)
    {
        TerrainReport report("bake", {check::kGenerate});
        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(tiles);
        return std::move(*tiles);
    }

    TerrainRegionBake Regions(const TerrainRecipe &recipe, const std::vector<TileHeights> &tiles,
                              TerrainRegionSettings settings)
    {
        std::string error;
        std::optional<TerrainRegionBake> bake = BakeTerrainRegions(recipe, tiles, settings, error);
        INFO(error);
        REQUIRE(bake);
        return std::move(*bake);
    }

    TerrainBakedRelease Bake(const TerrainRecipe &recipe, TerrainRegionSettings settings)
    {
        TerrainReport report("bake", {check::kGenerate, check::kEncode});
        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        REQUIRE(tiles);
        std::optional<TerrainBakedRelease> baked =
            EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe, std::move(*tiles),
                               {kNonce, {}, settings}, report);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(baked);
        return std::move(*baked);
    }

    TerrainFinalSurface Surface(const TerrainRecipe &recipe, const std::vector<TileHeights> &tiles)
    {
        TerrainFinalSurface final{recipe, {}};
        for (const TileHeights &tile : tiles)
            final.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile);
        return final;
    }

    using Point = std::pair<double, double>;
    struct Node
    {
        float height = 0.0f;
        glm::vec3 normal{0.0f};
    };

    void Collect(std::map<Point, Node> &into, const glm::dvec3 &origin, std::span<const glm::vec3> positions,
                 std::span<const glm::vec3> normals)
    {
        for (size_t i = 0; i < positions.size(); ++i)
            into[{origin.x + positions[i].x, origin.z + positions[i].z}] = {positions[i].y, normals[i]};
    }

    // Nodes on the line x = fixed (or z = fixed) between from and to, inclusive.
    std::map<Point, Node> OnLine(const std::map<Point, Node> &nodes, bool vertical, double fixed, double from,
                                 double to)
    {
        std::map<Point, Node> line;
        for (const auto &[p, n] : nodes)
        {
            const double a = vertical ? p.first : p.second, b = vertical ? p.second : p.first;
            if (a == fixed && b >= from && b <= to)
                line.emplace(p, n);
        }
        return line;
    }

    bool SameNodes(const std::map<Point, Node> &a, const std::map<Point, Node> &b)
    {
        if (a.size() != b.size())
            return false;
        for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j)
            if (i->first != j->first || std::bit_cast<uint32_t>(i->second.height) != std::bit_cast<uint32_t>(j->second.height) ||
                i->second.normal != j->second.normal)
                return false;
        return true;
    }

    uint64_t PayloadsHash(const TerrainRegionBake &bake)
    {
        std::vector<std::byte> all;
        for (const TerrainPayloadWrite &payload : bake.writes.payloads)
            all.insert(all.end(), payload.bytes.begin(), payload.bytes.end());
        return Fnv1a64(all);
    }

    fs::path WriteRecipe(const fs::path &dir, const TerrainRecipe &recipe)
    {
        nlohmann::json json = nlohmann::json::parse(*ReadText(ExampleRecipePath()));
        json["recipeVersion"] = 2;
        json["generator"]["version"] = 2;
        json["layout"] = {{"sampleSpacing", recipe.layout.SampleSpacing()},
                          {"intervalsPerTile", recipe.layout.IntervalsPerTile()},
                          {"bounds", {{"minX", recipe.layout.Bounds().minX}, {"minZ", recipe.layout.Bounds().minZ},
                                      {"maxX", recipe.layout.Bounds().maxX}, {"maxZ", recipe.layout.Bounds().maxZ}}}};
        json["parameters"]["ridges"] = {{"amplitude", recipe.relief.ridges.amplitude},
                                        {"wavelength", recipe.relief.ridges.wavelength},
                                        {"octaves", recipe.relief.ridges.octaves}};
        json["parameters"]["detail"] = {{"amplitude", recipe.relief.detail.amplitude},
                                        {"wavelength", recipe.relief.detail.wavelength},
                                        {"octaves", recipe.relief.detail.octaves}};
        json["parameters"]["plains"]["height"] = recipe.relief.plainsHeight;
        nlohmann::json tiles = nlohmann::json::array();
        for (const TerrainSourceRule &rule : recipe.sourceRules)
            tiles.push_back({{"x", rule.tile.x}, {"z", rule.tile.z}, {"spacing", rule.spacing}});
        json["precision"] = {{"defaultSpacing", *recipe.defaultSpacing}, {"tiles", tiles}};
        fs::create_directories(dir);
        const fs::path path = dir / "relief-2x2.terrain.json";
        REQUIRE(WriteText(path, json.dump(2)));
        return path;
    }

    // Edits one Region payload and its record, then reseals every checksum so only the edit is wrong.
    void PatchRegionSealed(const fs::path &dir, size_t ordinal, const std::function<void(std::vector<std::byte> &)> &edit,
                           const std::function<void(std::vector<std::byte> &, size_t, TerrainTileRecord &)> &editRecord)
    {
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainIndex parsed = *ParseTerrainIndex(index).value;
        const TerrainTileRecord &record = parsed.regions[ordinal];
        const std::string pack = TerrainPackFileName(parsed.packs[record.pack].chunk);
        std::vector<std::byte> bytes = ReadFile(dir / pack);
        std::vector<std::byte> payload(bytes.begin() + record.offset, bytes.begin() + record.offset + record.length);
        edit(payload);
        std::copy(payload.begin(), payload.end(), bytes.begin() + record.offset);

        const size_t at = kTerrainIndexHeaderSize + parsed.packs.size() * kTerrainPackRecordSize +
                          parsed.tiles.size() * kTerrainTileRecordSize +
                          parsed.layers.size() * kTerrainLayerRecordSize + ordinal * kTerrainTileRecordSize;
        std::vector<TerrainTileRecord> regions = parsed.regions;
        editRecord(index, at, regions[ordinal]);
        StoreU64(index, at + 32, Fnv1a64(payload));
        StoreU64(index, kTerrainIndexHeaderSize + record.pack * kTerrainPackRecordSize + 24, Fnv1a64(bytes));
        regions[ordinal].checksum = Fnv1a64(payload);
        StoreU64(index, 112, TerrainRecordsHeightsHash(parsed.tiles, regions));
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));
        WriteFile(dir / pack, bytes);
        WriteFile(dir / kTerrainIndexFileName, index);
    }

    TerrainReport ValidateDir(const fs::path &dir, const TerrainRecipe *recipe = nullptr)
    {
        TerrainReport report("validate", {check::kRegions});
        ValidateTerrainCandidate(dir, {recipe, true}, report);
        return report;
    }
}

TEST_CASE("Regions cover their valid extent and bound their distance to the final mixed surface",
          "[terraintool][regions]")
{
    const TerrainRecipe recipe = MixedRecipe();
    const std::vector<TileHeights> tiles = FinalHeights(recipe);
    const TerrainFinalSurface final = Surface(recipe, tiles);
    for (const TerrainRegionSettings settings : {TerrainRegionSettings{1, 4}, TerrainRegionSettings{2, 8},
                                                 TerrainRegionSettings{}})
    {
        INFO("Region tiles " << settings.regionTiles << ", cells " << settings.cells);
        const TerrainRegionBake bake = Regions(recipe, tiles, settings);
        const TerrainRegionGrid &grid = *bake.writes.grid;
        CHECK(grid.Unit() == 8);
        REQUIRE(bake.meshes.size() == grid.RegionCount());
        std::mt19937 random(124);
        for (const TerrainRegionMesh &mesh : bake.meshes)
        {
            INFO("Region L" << mesh.region.level << " (" << mesh.region.x << ", " << mesh.region.z << ")");
            CHECK(TerrainRegionCoversDomain(grid, mesh));
            CHECK(mesh.maxDeviation > 0.0);
            const glm::dvec3 origin = mesh.origin.Meters();
            for (size_t i = 0; i < mesh.positions.size(); ++i)
            {
                const double x = origin.x + mesh.positions[i].x, z = origin.z + mesh.positions[i].z;
                if (x < recipe.layout.Bounds().minX || x > recipe.layout.Bounds().maxX ||
                    z < recipe.layout.Bounds().minZ || z > recipe.layout.Bounds().maxZ)
                    continue;
                const TileHeights &owner = *final.Tile(AddressLocal(recipe.layout,
                    *TerrainLocalPosition::FromMeters({x, 0.0, z})).tile);
                CHECK(mesh.positions[i].y == float(final.SharedHeight(x, z, owner)));
                CHECK(mesh.normals[i] == final.Normal(x, z, owner));
            }
            const TerrainRegionSampler sampler(grid, mesh);
            const TerrainLocalBounds extent = grid.ValidExtent(mesh.region);
            std::uniform_real_distribution<double> ux(extent.minX, extent.maxX), uz(extent.minZ, extent.maxZ);
            double worst = 0.0;
            for (int probe = 0; probe < 400; ++probe)
            {
                const double x = ux(random), z = uz(random);
                const TileHeights &owner =
                    *final.Tile(AddressLocal(recipe.layout, *TerrainLocalPosition::FromMeters({x, 0.0, z})).tile);
                const std::optional<double> coarse = sampler.HeightAt(x - origin.x, z - origin.z);
                REQUIRE(coarse);
                CHECK(*coarse == EvaluateTerrainRegion(mesh, x - origin.x, z - origin.z));
                worst = std::max(worst, std::abs(*coarse - final.SharedHeight(x, z, owner)));
            }
            CHECK(worst <= mesh.maxDeviation);
        }
    }
}

TEST_CASE("Region edges hold the tile LOD ring nodes and their neighbours' nodes bit for bit",
          "[terraintool][regions]")
{
    const TerrainRecipe recipe = MixedRecipe();
    const TerrainBakedRelease baked = Bake(recipe, {1, 4});
    const TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(baked.encoded));
    REQUIRE(opened);
    const TerrainReleaseHandle &handle = *opened.value;
    const TerrainIndex &index = handle.Index();
    REQUIRE(index.regionGrid);
    const TerrainRegionGrid &grid = *index.regionGrid;
    REQUIRE(grid.Levels() == 3);

    std::map<std::pair<int32_t, int32_t>, std::vector<std::map<Point, Node>>> rings;
    for (const TerrainTileRecord &record : index.tiles)
    {
        if (record.kind != TerrainPayloadKind::LodHeight)
            continue;
        const TerrainResult<TerrainLodSurface> lod = handle.ReadLod({index.release.asset, record.tile}, record.layer);
        const TerrainResult<TerrainLodNormals> normals =
            handle.ReadNormals({index.release.asset, record.tile}, record.layer);
        REQUIRE(lod);
        REQUIRE(normals);
        std::map<Point, Node> nodes;
        Collect(nodes, lod.value->origin.Meters(), lod.value->positions, normals.value->normals);
        rings[{record.tile.x, record.tile.z}].push_back(std::move(nodes));
    }

    std::vector<std::map<Point, Node>> regions;
    for (uint64_t ordinal = 0; ordinal < grid.RegionCount(); ++ordinal)
    {
        const TerrainResult<TerrainRegionMesh> mesh = handle.ReadRegion(grid.AtOrdinal(ordinal));
        INFO(mesh.error.message);
        REQUIRE(mesh);
        std::map<Point, Node> nodes;
        Collect(nodes, mesh.value->origin.Meters(), mesh.value->positions, mesh.value->normals);
        regions.push_back(std::move(nodes));
    }

    size_t tileEdges = 0, regionEdges = 0;
    const TerrainLayout &ref = recipe.layout;
    for (uint64_t ordinal = 0; ordinal < grid.RegionCount(); ++ordinal)
    {
        const TerrainRegionCoord region = grid.AtOrdinal(ordinal);
        INFO("Region L" << region.level << " (" << region.x << ", " << region.z << ")");
        const TileRange tiles = grid.MapTiles(region);
        const TerrainLocalBounds extent = grid.ValidExtent(region);
        const TileCoord count = grid.Count(region.level);
        struct Side
        {
            bool inside;
            bool vertical;
            double fixed;
            int32_t outside;
        };
        const std::array<Side, 4> sides{{{region.x > 0, true, extent.minX, tiles.min.x - 1},
                                          {region.x < count.x - 1, true, extent.maxX, tiles.max.x + 1},
                                          {region.z > 0, false, extent.minZ, tiles.min.z - 1},
                                          {region.z < count.z - 1, false, extent.maxZ, tiles.max.z + 1}}};
        for (const Side &side : sides)
        {
            if (!side.inside)
                continue;
            const int32_t first = side.vertical ? tiles.min.z : tiles.min.x;
            const int32_t last = side.vertical ? tiles.max.z : tiles.max.x;
            for (int32_t along = first; along <= last; ++along)
            {
                const TileCoord neighbour = side.vertical ? TileCoord{side.outside, along} : TileCoord{along, side.outside};
                const TileCoord mineTile = side.vertical ? TileCoord{side.outside == tiles.min.x - 1 ? tiles.min.x : tiles.max.x, along}
                                                         : TileCoord{along, side.outside == tiles.min.z - 1 ? tiles.min.z : tiles.max.z};
                const TerrainLocalBounds segment = *ref.ValidExtent(neighbour);
                const double from = side.vertical ? segment.minZ : segment.minX;
                const double to = side.vertical ? segment.maxZ : segment.maxX;
                const std::map<Point, Node> mine = OnLine(regions[ordinal], side.vertical, side.fixed, from, to);
                CHECK(mine.size() >= 2);
                for (const std::map<Point, Node> &ring : rings.at({neighbour.x, neighbour.z}))
                {
                    CHECK(SameNodes(mine, OnLine(ring, side.vertical, side.fixed, from, to)));
                    ++tileEdges;
                }
                for (uint32_t level = 0; level < grid.Levels(); ++level)
                {
                    const TerrainRegionCoord other = grid.Containing(level, neighbour);
                    if (other == grid.Containing(level, mineTile))
                        continue;
                    CHECK(SameNodes(mine, OnLine(regions[grid.Ordinal(other)], side.vertical, side.fixed, from, to)));
                    ++regionEdges;
                }
            }
        }
    }
    CHECK(tileEdges > 0);
    CHECK(regionEdges > 0);
}

TEST_CASE("A repeated Region bake gives the same bytes in every build", "[terraintool][regions]")
{
    const TerrainRecipe recipe = MixedRecipe();
    const std::vector<TileHeights> tiles = FinalHeights(recipe);
    const TerrainRegionBake a = Regions(recipe, tiles, {1, 4});
    const TerrainRegionBake b = Regions(recipe, tiles, {1, 4});
    REQUIRE(a.writes.payloads.size() == b.writes.payloads.size());
    for (size_t i = 0; i < a.writes.payloads.size(); ++i)
        CHECK(a.writes.payloads[i].bytes == b.writes.payloads[i].bytes);
    CHECK(PayloadsHash(a) == 0x293defc91aa9b524ull);
    CHECK(PayloadsHash(Regions(recipe, tiles, {})) == 0x589253572ebd441eull);
}

TEST_CASE("Validation rederives every Region and refuses forged or missing ones", "[terraintool][regions][validate]")
{
    ScratchDir scratch("regions_validate");
    const TerrainRecipe recipe = MixedRecipe();
    TerrainBakeRequest request = ExampleRequest(scratch.path / "candidate");
    request.recipePath = WriteRecipe(scratch.path / "recipe", recipe);
    request.options.regions = {1, 4};
    const TerrainReport baked = RunTerrainBake(request);
    for (const TerrainDiagnostic &d : baked.diagnostics)
        UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(baked.Ok());
    CHECK(baked.Status(check::kRegions) == CheckStatus::Passed);
    REQUIRE(baked.regionGrid);
    CHECK(baked.regionGrid->levels == 3);
    CHECK(baked.regionGrid->levelZeroEdge == 16.0);
    REQUIRE(baked.regions.size() == 21);
    for (const TerrainRegionSummary &region : baked.regions)
    {
        CHECK(region.readable);
        CHECK(region.maxDeviation > 0.0);
        CHECK(region.vertices >= 4);
    }
    const fs::path dir = request.candidateDir;

    SECTION("an intact candidate")
    {
        CHECK(ValidateDir(dir).Status(check::kRegions) == CheckStatus::Passed);
    }
    SECTION("a resealed smaller error")
    {
        PatchRegionSealed(
            dir, 5, [](std::vector<std::byte> &payload) { StoreU64(payload, 88, std::bit_cast<uint64_t>(0.001)); },
            [](std::vector<std::byte> &index, size_t at, TerrainTileRecord &record) {
                StoreU64(index, at + 112, std::bit_cast<uint64_t>(0.001));
                record.maxDeviation = 0.001;
            });
        const TerrainReport report = ValidateDir(dir);
        const std::vector<TerrainDiagnostic> found = DiagnosticsOf(report, check::kRegions);
        REQUIRE(found.size() == 1);
        CHECK(found[0].message.find("final-source derivation") != std::string::npos);
        CHECK(report.Status(check::kIndex) == CheckStatus::Passed);
    }
    SECTION("a resealed raised vertex")
    {
        PatchRegionSealed(
            dir, 20, [](std::vector<std::byte> &payload) { payload[kTerrainRegionHeaderSize + 4] ^= std::byte{1}; },
            [](std::vector<std::byte> &, size_t, TerrainTileRecord &) {});
        const std::vector<TerrainDiagnostic> found = DiagnosticsOf(ValidateDir(dir), check::kRegions);
        REQUIRE_FALSE(found.empty());
    }
    SECTION("a missing pack")
    {
        const TerrainIndex index = *ParseTerrainIndex(ReadFile(dir / kTerrainIndexFileName)).value;
        const std::string pack = TerrainPackFileName(index.packs[index.regions.back().pack].chunk);
        fs::remove(dir / pack);
        const TerrainReport report = ValidateDir(dir);
        CHECK(report.Status(check::kRegions) == CheckStatus::Failed);
        bool named = false;
        for (const TerrainDiagnostic &d : DiagnosticsOf(report, check::kRegions))
            named = named || (d.field == pack && d.message.find("Region L2 (0, 0)") != std::string::npos);
        CHECK(named);
    }
}

TEST_CASE("Validation requires Regions only from a recipe-backed candidate", "[terraintool][regions][validate]")
{
    ScratchDir scratch("regions_height_only");
    const fs::path dir = scratch.path / "candidate";
    WriteCraftedCandidate(dir, [](auto &) {});
    const TerrainRecipe recipe = LoadExample();
    CHECK(ValidateDir(dir).Status(check::kRegions) == CheckStatus::Skipped);
    const TerrainReport backed = ValidateDir(dir, &recipe);
    CHECK(backed.Status(check::kRegions) == CheckStatus::Failed);
    CHECK_FALSE(DiagnosticsOf(backed, check::kRegions).empty());
}

TEST_CASE("Sync republishes a current release without this tool's Region hierarchy", "[terraintool][regions][sync]")
{
    ScratchDir scratch("regions_sync");
    const TerrainRecipe recipe = MixedRecipe();
    const fs::path recipePath = WriteRecipe(scratch.path / "recipe", recipe);
    const fs::path releases = scratch.path / "releases";
    const AssetId asset = MakeAssetId(ExampleAssetPath());
    for (const TerrainRegionSettings settings : {TerrainRegionSettings{1, 4}, TerrainRegionSettings{}})
    {
        const fs::path candidate = scratch.path / ("candidate" + std::to_string(settings.cells));
        TerrainBakeRequest request = ExampleRequest(candidate, std::nullopt);
        request.recipePath = recipePath;
        request.options.regions = settings;
        REQUIRE(RunTerrainBake(request).Ok());
        REQUIRE(RunTerrainPublish({candidate, releases, recipePath}).Ok());
        std::optional<TerrainReleaseId> current;
        const std::optional<std::string> mismatch =
            TerrainCurrentMismatch(releases, asset, FingerprintRecipe(recipe), current);
        if (settings == TerrainRegionSettings{})
            CHECK_FALSE(mismatch);
        else
        {
            REQUIRE(mismatch);
            CHECK(mismatch->find("Region hierarchy") != std::string::npos);
        }
    }
}
