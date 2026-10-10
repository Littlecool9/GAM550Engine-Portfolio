/**
 * @file TerrainRegionExperiment.cpp
 * @author Jackey Cen
 * @brief Compares Region level sizes and grid resolutions on one final surface: storage, vertices and error.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <TerrainBake.h>
#include <TerrainFinalSurface.h>
#include <TerrainRegions.h>

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainLod.h>
#include <terrain/TileAddressing.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using Json = nlohmann::ordered_json;

namespace
{
    double Seconds(std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    }

    double Percentile(std::vector<double> values, double p)
    {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, size_t(p * double(values.size() - 1) + 0.5))];
    }

    // Vertices on the edges a Region shares with a neighbour, which carry the source nodes.
    size_t BoundaryVertices(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh)
    {
        const TerrainLayout &ref = grid.Reference();
        const TerrainLocalBounds rect = grid.Rect(mesh.region);
        const SampleRange domain = grid.Domain(mesh.region);
        const TileCoord count = grid.Count(mesh.region.level);
        const float left = float(ref.SamplePosition(domain.min.x) - rect.minX);
        const float right = float(ref.SamplePosition(domain.max.x) - rect.minX);
        const float bottom = float(ref.SamplePosition(domain.min.z) - rect.minZ);
        const float top = float(ref.SamplePosition(domain.max.z) - rect.minZ);
        size_t boundary = 0;
        for (const glm::vec3 &p : mesh.positions)
            boundary += (mesh.region.x > 0 && p.x == left) || (mesh.region.x < count.x - 1 && p.x == right) ||
                        (mesh.region.z > 0 && p.z == bottom) || (mesh.region.z < count.z - 1 && p.z == top);
        return boundary;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: terrain_region_experiment recipe out-directory [samples per Region]\n";
        return 2;
    }
    const RecipeParseResult parsed = LoadTerrainRecipe(argv[1]);
    if (!parsed.recipe)
    {
        for (const RecipeIssue &issue : parsed.issues)
            std::cerr << issue.field << ": " << issue.message << '\n';
        return 1;
    }
    const std::filesystem::path out(argv[2]);
    const int samples = argc > 3 ? std::stoi(argv[3]) : 2000;
    std::filesystem::create_directories(out);
    const TerrainRecipe &recipe = *parsed.recipe;

    TerrainReport report("bake", {check::kGenerate});
    auto start = std::chrono::steady_clock::now();
    const std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
    if (!tiles)
    {
        for (const TerrainDiagnostic &d : report.diagnostics)
            std::cerr << FormatDiagnostic(d) << '\n';
        return 1;
    }
    const double heightsSeconds = Seconds(start);
    std::map<double, std::pair<size_t, uint64_t>> sources;
    for (const TileHeights &tile : *tiles)
    {
        auto &entry = sources[*recipe.SourceSpacing(tile.Tile())];
        ++entry.first;
        entry.second += uint64_t(tile.Samples().size()) * sizeof(float);
    }
    TerrainFinalSurface final{recipe, {}};
    for (const TileHeights &tile : *tiles)
        final.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile);

    Json summary;
    summary["recipe"] = argv[1];
    summary["tiles"] = tiles->size();
    summary["tileSize"] = recipe.layout.TileSize();
    summary["finalHeightsSeconds"] = heightsSeconds;
    summary["sources"] = Json::array();
    uint64_t sourceBytes = 0;
    for (const auto &[spacing, entry] : sources)
    {
        summary["sources"].push_back({{"spacing", spacing}, {"tiles", entry.first}, {"heightBytes", entry.second}});
        sourceBytes += entry.second;
    }
    summary["sourceHeightBytes"] = sourceBytes;
    summary["configs"] = Json::array();

    std::string csv = "regionTiles,levelZeroEdge,cells,level,regions,cellSpacing,vertices,boundaryVertices,"
                      "maxVertices,triangles,payloadBytes,maxRegionBytes,maxError,meanRegionError,sampledP50,"
                      "sampledP95,sampledP99,sampledMax,bakeSeconds\n";
    for (const uint32_t regionTiles : {16u, 32u})
        for (const uint32_t cells : {64u, 128u, 256u})
        {
            std::string error;
            start = std::chrono::steady_clock::now();
            const std::optional<TerrainRegionBake> bake =
                BakeTerrainRegions(recipe, *tiles, {regionTiles, cells}, error);
            const double bakeSeconds = Seconds(start);
            if (!bake)
            {
                std::cerr << regionTiles << " x " << cells << ": " << error << '\n';
                return 1;
            }
            const std::optional<TerrainRegionBake> again =
                BakeTerrainRegions(recipe, *tiles, {regionTiles, cells}, error);
            std::vector<std::byte> all, repeated;
            for (const TerrainPayloadWrite &p : bake->writes.payloads)
                all.insert(all.end(), p.bytes.begin(), p.bytes.end());
            for (const TerrainPayloadWrite &p : again->writes.payloads)
                repeated.insert(repeated.end(), p.bytes.begin(), p.bytes.end());

            const TerrainRegionGrid &grid = *bake->writes.grid;
            const double edge = recipe.layout.TileSize() * regionTiles;
            Json config = {{"regionTiles", regionTiles}, {"levelZeroEdge", edge}, {"cells", cells},
                           {"unit", grid.Unit()},          {"levels", grid.Levels()},
                           {"regions", grid.RegionCount()}, {"bakeSeconds", bakeSeconds},
                           {"payloadHash", std::format("{:016x}", Fnv1a64(all))},
                           {"repeatsBitForBit", all == repeated}, {"levelStats", Json::array()}};
            uint64_t totalBytes = 0;
            std::mt19937 random(124);
            for (uint32_t level = 0; level < grid.Levels(); ++level)
            {
                size_t regions = 0, vertices = 0, boundary = 0, maxVertices = 0, triangles = 0;
                uint64_t bytes = 0, maxBytes = 0;
                double maxError = 0.0, errorSum = 0.0;
                std::vector<double> sampled;
                for (size_t i = 0; i < bake->meshes.size(); ++i)
                {
                    const TerrainRegionMesh &mesh = bake->meshes[i];
                    if (mesh.region.level != level)
                        continue;
                    ++regions;
                    vertices += mesh.positions.size();
                    boundary += BoundaryVertices(grid, mesh);
                    maxVertices = std::max(maxVertices, mesh.positions.size());
                    triangles += mesh.indices.size() / 3;
                    bytes += bake->writes.payloads[i].bytes.size();
                    maxBytes = std::max<uint64_t>(maxBytes, bake->writes.payloads[i].bytes.size());
                    maxError = std::max(maxError, mesh.maxDeviation);
                    errorSum += mesh.maxDeviation;
                    const TerrainRegionSampler sampler(grid, mesh);
                    const TerrainLocalBounds extent = grid.ValidExtent(mesh.region);
                    const glm::dvec3 origin = mesh.origin.Meters();
                    std::uniform_real_distribution<double> ux(extent.minX, extent.maxX), uz(extent.minZ, extent.maxZ);
                    for (int s = 0; s < samples; ++s)
                    {
                        const double x = ux(random), z = uz(random);
                        const TileAddress address =
                            AddressLocal(recipe.layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}));
                        const std::optional<double> coarse = sampler.HeightAt(x - origin.x, z - origin.z);
                        if (!address.IsInside() || !coarse)
                            continue;
                        sampled.push_back(std::abs(*coarse - final.SharedHeight(x, z, *final.Tile(address.tile))));
                    }
                }
                totalBytes += bytes;
                const Json stats = {{"level", level},
                                    {"regions", regions},
                                    {"cellSpacing", grid.CellSpacing(level)},
                                    {"vertices", vertices},
                                    {"boundaryVertices", boundary},
                                    {"maxVertices", maxVertices},
                                    {"triangles", triangles},
                                    {"payloadBytes", bytes},
                                    {"maxRegionBytes", maxBytes},
                                    {"maxError", maxError},
                                    {"meanRegionError", regions ? errorSum / double(regions) : 0.0},
                                    {"sampledP50", Percentile(sampled, 0.5)},
                                    {"sampledP95", Percentile(sampled, 0.95)},
                                    {"sampledP99", Percentile(sampled, 0.99)},
                                    {"sampledMax", Percentile(sampled, 1.0)}};
                config["levelStats"].push_back(stats);
                csv += std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", regionTiles, edge,
                                   cells, level, regions, grid.CellSpacing(level), vertices, boundary, maxVertices,
                                   triangles, bytes, maxBytes, maxError, stats["meanRegionError"].get<double>(),
                                   stats["sampledP50"].get<double>(), stats["sampledP95"].get<double>(),
                                   stats["sampledP99"].get<double>(), stats["sampledMax"].get<double>(), bakeSeconds);
            }
            config["payloadBytes"] = totalBytes;
            config["payloadToSourceRatio"] = double(totalBytes) / double(sourceBytes);
            summary["configs"].push_back(config);
            std::cout << std::format("{} tiles x {} cells: {} Regions in {} levels, {} bytes, {:.2f} s\n", regionTiles,
                                     cells, grid.RegionCount(), grid.Levels(), totalBytes, bakeSeconds);
        }
    if (!WriteText(out / "regions.csv", csv) || !WriteText(out / "regions.json", summary.dump(2) + "\n"))
        return 1;
    return 0;
}
