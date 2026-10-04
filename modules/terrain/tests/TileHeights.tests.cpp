/**
 * @file TileHeights.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TileHeights.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("TileHeights holds exactly (n + 1)^2 samples", "[terrain][heights]")
{
    const auto zero = TileHeights::Create({-3, 7}, 4);
    REQUIRE(zero);
    CHECK(zero->Tile() == TileCoord{-3, 7});
    CHECK(zero->SamplesPerSide() == 5);
    CHECK(zero->Samples().size() == 25);
    for (float h : zero->Samples())
        CHECK(h == 0.0f);

    CHECK_FALSE(TileHeights::Create({0, 0}, 0));
    CHECK_FALSE(TileHeights::Create({0, 0}, -1));
    CHECK_FALSE(TileHeights::Create({0, 0}, kMaxTerrainTileIntervals + 1));
    CHECK(TileHeights::Create({0, 0}, 2, std::vector<float>(9, 1.0f)));
    CHECK_FALSE(TileHeights::Create({0, 0}, 2, std::vector<float>(8, 1.0f)));
    CHECK_FALSE(TileHeights::Create({0, 0}, 2, std::vector<float>(16, 1.0f)));
}

TEST_CASE("TileHeights is row-major with x fastest", "[terrain][heights]")
{
    std::vector<float> samples(9);
    for (size_t s = 0; s < samples.size(); ++s)
        samples[s] = static_cast<float>(s);
    auto tile = TileHeights::Create({1, -1}, 2, samples);
    REQUIRE(tile);

    // index = k * (n + 1) + i
    CHECK(tile->At(0, 0) == 0.0f);
    CHECK(tile->At(2, 0) == 2.0f);
    CHECK(tile->At(0, 1) == 3.0f);
    CHECK(tile->At(1, 2) == 7.0f);

    tile->Set(2, 1, -4.5f);
    CHECK(tile->Samples()[5] == -4.5f);
}
