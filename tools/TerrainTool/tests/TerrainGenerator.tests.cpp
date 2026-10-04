/**
 * @file TerrainGenerator.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainGenerator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ReliefMetrics.h"

#include <TerrainGenerator.h>
#include <TerrainNoise.h>
#include <TerrainRecipe.h>
#include <assets/AssetId.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <fstream>
#include <map>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using Json = nlohmann::json;

namespace
{
    Json Example()
    {
        std::ifstream in(std::string(TERRAIN_FIXTURE_DIR) + "/relief-2x2.terrain.json");
        return Json::parse(in);
    }

    TerrainRecipe Valid(const Json &json)
    {
        RecipeParseResult result = ParseTerrainRecipe(json.dump());
        for (const RecipeIssue &issue : result.issues)
            UNSCOPED_INFO(issue.field << ": " << issue.message);
        REQUIRE(result.recipe);
        return *result.recipe;
    }

    TerrainRecipe WithValue(const char *pointer, const Json &value)
    {
        Json edited = Example();
        edited[Json::json_pointer(pointer)] = value;
        return Valid(edited);
    }

    std::vector<TileCoord> MapTiles(const TerrainLayout &layout)
    {
        std::vector<TileCoord> tiles;
        for (int32_t z = layout.Tiles().min.z; z <= layout.Tiles().max.z; ++z)
            for (int32_t x = layout.Tiles().min.x; x <= layout.Tiles().max.x; ++x)
                tiles.push_back({x, z});
        return tiles;
    }

    std::vector<float> Tile(const TerrainGenerator &generator, TileCoord tile)
    {
        std::vector<float> block(TileSampleCount(generator.Layout()));
        REQUIRE(generator.FillTile(tile, block).Ok());
        return block;
    }

    // Global sample -> float bits; a shared sample must match in every tile.
    using SampleBits = std::map<std::pair<int64_t, int64_t>, uint32_t>;

    void Collect(SampleBits &samples, const TerrainLayout &layout, TileCoord tile, const std::vector<float> &block)
    {
        const int64_t n = layout.IntervalsPerTile();
        const int64_t i0 = layout.TileFirstSample(tile.x);
        const int64_t k0 = layout.TileFirstSample(tile.z);
        for (int64_t k = k0; k <= k0 + n; ++k)
            for (int64_t i = i0; i <= i0 + n; ++i)
            {
                const uint32_t bits = std::bit_cast<uint32_t>(block[static_cast<size_t>((k - k0) * (n + 1) + (i - i0))]);
                const auto [it, inserted] = samples.emplace(std::pair{i, k}, bits);
                if (!inserted)
                    CHECK(it->second == bits);
            }
    }

    uint64_t HashTiles(const TerrainGenerator &generator)
    {
        std::vector<std::byte> bytes;
        for (const TileCoord tile : MapTiles(generator.Layout()))
            for (const float height : Tile(generator, tile))
            {
                const uint32_t bits = std::bit_cast<uint32_t>(height);
                for (int shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xFFu));
            }
        return Fnv1a64(bytes);
    }
}

TEST_CASE("Gradient noise stays in range and vanishes on the lattice", "[terraintool][noise]")
{
    const OctaveNoise octaves(99, 16.0, 4);
    for (int k = -200; k <= 200; ++k)
        for (int i = -200; i <= 200; ++i)
        {
            const double x = i * 0.37;
            const double z = k * 0.53;
            const double n = GradientNoise(x, z, 7);
            REQUIRE(std::abs(n) <= 1.0);
            REQUIRE(GradientNoise(x, z, 7) == n);
            const double fbm = octaves.Fbm(x * 8.0, z * 8.0);
            REQUIRE(std::abs(fbm) <= 1.0);
            const double ridged = octaves.Ridged(x * 8.0, z * 8.0);
            REQUIRE(ridged >= 0.0);
            REQUIRE(ridged <= 1.0);
        }
    CHECK(GradientNoise(3.0, -5.0, 7) == 0.0);
    CHECK(GradientNoise(3.5, -5.5, 7) != GradientNoise(3.5, -5.5, 8));
    // Per-octave offsets keep the origin off the lattice of every octave.
    CHECK(octaves.Fbm(0.0, 0.0) != 0.0);
}

TEST_CASE("Repeating a recipe repeats every sample and the fingerprint", "[terraintool][generator]")
{
    const TerrainRecipe first = Valid(Example());
    const TerrainRecipe second = Valid(Example());
    CHECK(FingerprintRecipe(first) == FingerprintRecipe(second));

    const TerrainGenerator a(first);
    const TerrainGenerator b(second);
    for (const TileCoord tile : MapTiles(a.Layout()))
    {
        const std::vector<float> left = Tile(a, tile);
        const std::vector<float> right = Tile(b, tile);
        REQUIRE(left.size() == right.size());
        for (size_t s = 0; s < left.size(); ++s)
            REQUIRE(std::bit_cast<uint32_t>(left[s]) == std::bit_cast<uint32_t>(right[s]));
    }
    CHECK(HashTiles(a) == HashTiles(b));

    const TerrainGenerator other(WithValue("/seed", 20261002u));
    CHECK(HashTiles(other) != HashTiles(a));
}

TEST_CASE("Tile order and grouping do not change shared samples", "[terraintool][generator]")
{
    const TerrainRecipe recipe = Valid(Example());
    const TerrainLayout &layout = recipe.layout;
    std::vector<TileCoord> tiles = MapTiles(layout);
    REQUIRE(tiles.size() == 4);

    SampleBits forward;
    {
        const TerrainGenerator generator(recipe);
        for (const TileCoord tile : tiles)
            Collect(forward, layout, tile, Tile(generator, tile));
    }
    // 4 x 65^2 blocks minus the shared edges and corner.
    CHECK(forward.size() == 129u * 129u);

    SampleBits reversed;
    for (auto it = tiles.rbegin(); it != tiles.rend(); ++it)
    {
        const TerrainGenerator alone(recipe);
        Collect(reversed, layout, *it, Tile(alone, *it));
    }
    CHECK(reversed == forward);

    const TerrainGenerator generator(recipe);
    const int64_t n = layout.IntervalsPerTile();
    const auto at = [&](TileCoord tile, int64_t i, int64_t k)
    {
        const std::vector<float> block = Tile(generator, tile);
        const int64_t local = (k - layout.TileFirstSample(tile.z)) * (n + 1) + (i - layout.TileFirstSample(tile.x));
        return std::bit_cast<uint32_t>(block[static_cast<size_t>(local)]);
    };
    const uint32_t corner = std::bit_cast<uint32_t>(generator.SampleHeight({0, 0}));
    CHECK(at({-1, -1}, 0, 0) == corner);
    CHECK(at({0, -1}, 0, 0) == corner);
    CHECK(at({-1, 0}, 0, 0) == corner);
    CHECK(at({0, 0}, 0, 0) == corner);
    for (int64_t k = 0; k <= n; k += 7)
    {
        CHECK(at({-1, 0}, 0, k) == at({0, 0}, 0, k));
        CHECK(at({0, -1}, k, 0) == at({0, 0}, k, 0));
        CHECK(at({-1, -1}, 0, -k) == at({0, -1}, 0, -k));
    }

    std::vector<float> whole(129u * 129u);
    REQUIRE(generator.FillRange({{-64, -64}, {64, 64}}, whole).Ok());
    for (const auto &[index, bits] : forward)
        REQUIRE(std::bit_cast<uint32_t>(whole[static_cast<size_t>((index.second + 64) * 129 + (index.first + 64))]) ==
                bits);

    std::vector<float> corner3x5(3u * 5u);
    REQUIRE(generator.FillRange({{-1, -2}, {1, 2}}, corner3x5).Ok());
    for (int64_t k = -2; k <= 2; ++k)
        for (int64_t i = -1; i <= 1; ++i)
            CHECK(std::bit_cast<uint32_t>(corner3x5[static_cast<size_t>((k + 2) * 3 + (i + 1))]) ==
                  forward.at({i, k}));
}

TEST_CASE("A tile block is row-major with x fastest and keeps padding samples", "[terraintool][generator]")
{
    const TerrainRecipe recipe = Valid(Example());
    const TerrainGenerator generator(recipe);
    const TerrainLayout &layout = recipe.layout;
    const int64_t n = layout.IntervalsPerTile();
    REQUIRE(TileSampleCount(layout) == 65u * 65u);

    int padding = 0;
    for (const TileCoord tile : MapTiles(layout))
    {
        const std::vector<float> block = Tile(generator, tile);
        const int64_t i0 = layout.TileFirstSample(tile.x);
        const int64_t k0 = layout.TileFirstSample(tile.z);
        for (int64_t k = k0; k <= k0 + n; ++k)
            for (int64_t i = i0; i <= i0 + n; ++i)
            {
                const float height = block[static_cast<size_t>((k - k0) * (n + 1) + (i - i0))];
                REQUIRE(std::bit_cast<uint32_t>(height) == std::bit_cast<uint32_t>(generator.SampleHeight({i, k})));
                REQUIRE(height == static_cast<float>(generator.Height(layout.SamplePosition(i), layout.SamplePosition(k))));
                padding += layout.IsPaddingSample({i, k});
            }
    }
    // minX = -250 leaves samples x = -256, -252 outside; maxZ = 250 leaves z = 252, 256.
    CHECK(padding > 0);
}

TEST_CASE("Fills refuse tiles, ranges and spans that do not fit", "[terraintool][generator]")
{
    const TerrainGenerator generator(Valid(Example()));
    std::vector<float> block(TileSampleCount(generator.Layout()));
    CHECK(generator.FillTile({1, 0}, block).status == SourceFillStatus::OutsideMap);
    CHECK(generator.FillTile({0, -2}, block).status == SourceFillStatus::OutsideMap);

    std::vector<float> small(block.size() - 1);
    CHECK(generator.FillTile({0, 0}, small).status == SourceFillStatus::WrongSize);

    std::vector<float> one(1);
    CHECK(generator.FillRange({{3, 3}, {2, 3}}, one).status == SourceFillStatus::EmptyRange);
    CHECK(generator.FillRange({{65, 0}, {65, 0}}, one).status == SourceFillStatus::OutsideMap);
    CHECK(generator.FillRange({{-65, 0}, {-65, 0}}, one).status == SourceFillStatus::OutsideMap);
    CHECK(generator.FillRange({{64, -64}, {64, -64}}, one).Ok());
}

TEST_CASE("The 2x2 fixture holds every required shape with finite heights", "[terraintool][generator]")
{
    const MapGrid grid = GenerateMap(TerrainGenerator(Valid(Example())));
    REQUIRE(grid.width == 129);
    REQUIRE(grid.depth == 129);
    for (const float height : grid.heights)
        REQUIRE(std::isfinite(height));

    // Measured values are in ADR 0011.
    CHECK(CountFlatWindows(grid, 17) >= 1);             // flat: a 64 m square of equal heights
    CHECK(CountPlanarSlopes(grid, 9, 0.15, 0.25) >= 1); // slope: 32 m plane of >= 8.5 deg, rms <= 0.25 m
    CHECK(CountRidgeCrests(grid, 4, 0.15, 2.0) >= 20);  // ridge: about 80 m of sharp crest
    CHECK(BlockMeanSpread(grid, 16) >= 30.0);           // broad relief: 64 m block means span 30 m...
    CHECK(DetrendedBlockSpread(grid, 32) >= 5.0);       // ...and 128 m blocks are not one plane
    CHECK(CountCurvatureFlips(grid, 0.03) >= 3000);     // local detail: curvature flips every few samples
}

TEST_CASE("Each shape measure follows its own parameter", "[terraintool][generator]")
{
    const auto measure = [](const TerrainRecipe &recipe) { return GenerateMap(TerrainGenerator(recipe)); };

    // Relief and slopes have no such check: ridges also bend block means.
    CHECK(CountFlatWindows(measure(WithValue("/parameters/plains/height", -9000.0)), 17) == 0);
    CHECK(CountRidgeCrests(measure(WithValue("/parameters/ridges/amplitude", 0.0)), 4, 0.15, 2.0) == 0);
    CHECK(CountCurvatureFlips(measure(WithValue("/parameters/detail/amplitude", 0.0)), 0.03) < 3000);
}

TEST_CASE("The 2x2 fixture's samples are pinned", "[terraintool][generator]")
{
    // Same in Debug and Release; change only with kReliefGeneratorVersion.
    CHECK(HashTiles(TerrainGenerator(Valid(Example()))) == 0x36ce4ab9fb82fbfcull);
}
