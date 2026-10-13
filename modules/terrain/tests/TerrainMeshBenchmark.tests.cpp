/**
 * @file TerrainMeshBenchmark.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainTileMesh and cached payload measurement.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainRenderMesh.h>

#include "TerrainPackTestSupport.h"

#include <core/fs/FileIO.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>

using namespace mir;

TEST_CASE("Compare native mesh building with cached payloads", "[.][mesh-profile]")
{
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"fixture\":\"uniform flat terrain, center tile with neighbors resident\",\"cases\":[";
    bool first = true;
    for (const double spacing : {1.0, 8.0})
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, spacing, {0.0, 0.0, 768.0, 768.0});
        REQUIRE(layout);
        auto tiles = test::BuildTiles(*layout);
        for (auto &tile : tiles)
            for (int32_t z = 0; z <= layout->IntervalsPerTile(); ++z)
                for (int32_t x = 0; x <= layout->IntervalsPerTile(); ++x)
                    tile.Set(x, z, 10.0f);
        const auto payloads = test::BuildTestLods(std::span(&*layout, 1), tiles);
        std::vector<TerrainTileWrite> inputs;
        for (const auto &tile : tiles)
            inputs.push_back({&tile, 0, true});
        const auto info = test::Info(AssetId{4}, 5);
        const auto encoded = EncodeTerrainRelease(info, std::span(&*layout, 1), inputs, payloads);
        REQUIRE(encoded);
        auto opened = test::OpenInMemory(*encoded.value);
        const TileSampleLookup lookup = [&](TileCoord coord)
        {
            for (const auto &tile : tiles)
                if (tile.Tile() == coord)
                    return tile.Samples();
            return std::span<const float>{};
        };
        const TileKey key{info.asset, {1, 1}};
        const auto lod = opened.handle.ReadLod(key, 0);
        const auto normals = opened.handle.ReadNormals(key, 0);
        REQUIRE(lod);
        REQUIRE(normals);
        for (int iteration = 0; iteration < 50; ++iteration)
        {
            const auto begin = std::chrono::steady_clock::now();
            auto native = BuildTerrainTileMesh(*layout, info, key.coord, lookup);
            const auto built = std::chrono::steady_clock::now();
            auto cached = MakeTerrainRenderMesh(opened.handle.Index(), *lod.value, *normals.value);
            const auto assembled = std::chrono::steady_clock::now();
            auto encodedLod = opened.handle.ReadLod(key, 0);
            auto encodedNormals = opened.handle.ReadNormals(key, 0);
            const auto decoded = std::chrono::steady_clock::now();
            REQUIRE(native.IsOk());
            REQUIRE(cached);
            REQUIRE(encodedLod);
            REQUIRE(encodedNormals);
            CHECK(native.mesh->indices == cached.value->indices);
            REQUIRE(native.mesh->vertices.size() == cached.value->vertices.size());
            for (size_t i = 0; i < native.mesh->vertices.size(); ++i)
            {
                REQUIRE(native.mesh->vertices[i].position == cached.value->vertices[i].position);
                REQUIRE(native.mesh->vertices[i].normal == cached.value->vertices[i].normal);
            }
            if (!first)
                json << ',';
            first = false;
            json << "{\"spacing\":" << spacing << ",\"iteration\":" << iteration
                 << ",\"nativeMs\":" << std::chrono::duration<double, std::milli>(built - begin).count()
                 << ",\"cachedAssembleMs\":" << std::chrono::duration<double, std::milli>(assembled - built).count()
                 << ",\"cachedReadDecodeMs\":" << std::chrono::duration<double, std::milli>(decoded - assembled).count()
                 << ",\"vertices\":" << cached.value->vertices.size() << ",\"indices\":" << cached.value->indices.size() << '}';
        }
    }
    json << "]}\n";
    const char *output = std::getenv("MIR_TERRAIN_MESH_BENCHMARK");
    REQUIRE(output != nullptr);
    REQUIRE(WriteText(output, json.str()));
}
