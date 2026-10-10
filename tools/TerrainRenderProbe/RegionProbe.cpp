/**
 * @file RegionProbe.cpp
 * @author Jackey Cen
 * @brief Captures Region and tile selections from high-altitude and low-approach views for the size comparison.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "RegionProbe.h"

#include <TerrainBake.h>
#include <TerrainFinalSurface.h>
#include <TerrainRegions.h>

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <core/log/LogHistory.h>
#include <core/math/WorldReal.h>
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainRenderMesh.h>
#include <terrain/TileAddressing.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace mir;
    using namespace mir::terrainbake;
    using Json = nlohmann::json;

    void Require(bool value, const std::string &message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    std::vector<float> Floats(const std::filesystem::path &path)
    {
        const auto bytes = ReadBytes(path);
        Require(bytes && bytes->size() % sizeof(float) == 0, "missing GPU readback: " + path.string());
        std::vector<float> out(bytes->size() / sizeof(float));
        std::memcpy(out.data(), bytes->data(), bytes->size());
        return out;
    }

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

    // One draw unit: a tile at its source LOD or a whole Region.
    struct Unit
    {
        MeshHandle mesh;
        glm::dvec3 origin{0.0};
        TerrainLocalBounds rect;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        double error = 0.0;
        size_t vertices = 0;
        size_t triangles = 0;
    };

    Unit Upload(VulkanRenderer &renderer, const glm::dvec3 &origin, std::span<const glm::vec3> positions,
                std::span<const glm::vec3> normals, std::span<const uint32_t> indices)
    {
        std::vector<Vertex> vertices;
        vertices.reserve(positions.size());
        Unit unit;
        unit.origin = origin;
        unit.minHeight = std::numeric_limits<float>::max();
        unit.maxHeight = std::numeric_limits<float>::lowest();
        for (size_t i = 0; i < positions.size(); ++i)
        {
            vertices.push_back({.pos = positions[i], .color = glm::vec3(1), .normal = normals[i]});
            unit.minHeight = std::min(unit.minHeight, positions[i].y);
            unit.maxHeight = std::max(unit.maxHeight, positions[i].y);
        }
        unit.mesh = renderer.CreateMesh(vertices, indices);
        Require(!unit.mesh.IsNull(), "terrain upload failed");
        unit.vertices = positions.size();
        unit.triangles = indices.size() / 3;
        return unit;
    }

    struct View
    {
        std::string name;
        glm::dvec3 eye;
        glm::dvec3 target;
        // Every pixel must show terrain.
        bool covered = false;
    };

    struct Capture
    {
        std::vector<double> distance;
        size_t hits = 0;
    };
}

nlohmann::json RunRegionProbe(VulkanRenderer &renderer, Window &window, const std::filesystem::path &out,
                              uint32_t width, uint32_t height)
{
    std::filesystem::create_directories(out);
    const RecipeParseResult parsed = LoadTerrainRecipe(TERRAIN_REGION_RECIPE);
    Require(parsed.recipe.has_value(), "Region probe recipe cannot load");
    const TerrainRecipe &recipe = *parsed.recipe;

    auto start = std::chrono::steady_clock::now();
    TerrainReport report("bake", {check::kGenerate, check::kEncode});
    std::optional<std::vector<TileHeights>> heights = BakeFinalHeights(recipe, report);
    Require(heights.has_value(), "Region probe final heights failed");
    const std::vector<TileHeights> finalTiles = *heights;
    std::optional<TerrainBakedRelease> baked = EncodeBakedRelease(
        MakeAssetId("probe/regions.terrain.json"), FingerprintRecipe(recipe), recipe, std::move(*heights),
        {124, {}, {}}, report);
    if (!baked)
        for (const auto &d : report.diagnostics)
            std::cerr << FormatDiagnostic(d) << '\n';
    Require(baked.has_value(), "Region probe bake failed");
    const double bakeSeconds = Seconds(start);
    auto opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(baked->encoded));
    Require(bool(opened), opened.error.message);
    const TerrainReleaseHandle &handle = *opened.value;

    TerrainFinalSurface final{recipe, {}};
    for (const TileHeights &tile : finalTiles)
        final.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile);
    auto ground = [&](double x, double z) {
        const TileAddress a = AddressLocal(recipe.layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}));
        Require(a.IsInside(), "probe camera is off the map");
        return final.SharedHeight(x, z, *final.Tile(a.tile));
    };

    const glm::dvec3 placement = kLargeWorld ? glm::dvec3(150000.5, -20.0, -75000.25) : glm::dvec3(0.0, -20.0, 0.0);
    const TerrainLocalBounds bounds = recipe.layout.Bounds();
    const double cx = (bounds.minX + bounds.maxX) * 0.5, cz = (bounds.minZ + bounds.maxZ) * 0.5;
    const std::vector<View> views{
        {"high-altitude", {cx - 3000, ground(cx - 3000, cz - 2500) + 3000, cz - 2500},
         {cx + 4000, ground(cx + 4000, cz + 3000), cz + 3000}},
        {"low-approach", {cx - 5200, ground(cx - 5200, cz + 300) + 90, cz + 300},
         {cx - 2500, ground(cx - 2500, cz + 300), cz + 300}},
        {"nadir", {cx, ground(cx, cz) + 3500, cz}, {cx, ground(cx, cz), cz + 1e-3}, true}};

    renderer.SetDirectionalLight({-0.35f, 0.8f, -0.25f}, {0.95f, 0.9f, 0.8f});
    renderer.SetAmbientColor({0.25f, 0.28f, 0.32f});
    renderer.SetFog({0.62f, 0.7f, 0.8f}, 0.00004f);

    std::map<std::pair<int32_t, int32_t>, Unit> tiles;
    for (const TerrainTileRecord &record : handle.Index().tiles)
        if (record.source)
        {
            auto mesh = ReadTerrainRenderMesh(handle, record.tile);
            Require(bool(mesh), mesh.error.message);
            std::vector<glm::vec3> positions, normals;
            for (const TerrainMeshVertex &v : mesh.value->vertices)
            {
                positions.push_back(v.position);
                normals.push_back(v.normal);
            }
            Unit unit = Upload(renderer, mesh.value->origin.Meters(), positions, normals, mesh.value->indices);
            unit.rect = *recipe.layout.ValidExtent(record.tile);
            unit.error = 0.0;
            tiles.emplace(std::pair(record.tile.x, record.tile.z), unit);
        }

    auto draw = [&](const std::vector<const Unit *> &units, const View &view, const std::string &name, bool trace,
                    const Capture *reference, Json &result) {
        const WorldPosition eye = *WorldPosition::FromDoubleMeters(placement + view.eye);
        const glm::vec3 forward = glm::vec3(view.target - view.eye);
        const glm::mat4 orientation = glm::inverse(glm::lookAt(glm::vec3(0), forward, glm::vec3(0, 1, 0)));
        Camera lens;
        lens.mNearZ = 0.5f;
        lens.mFarZ = 0.0f;
        RenderView render;
        render.cameraPosition = eye;
        render.nearZ = lens.mNearZ;
        render.farZ = lens.mFarZ;
        render.viewProj = MakeCameraRelativeViewProjection({eye, glm::mat3(orientation)}, lens, float(width) / height);
        std::vector<WorldDrawItem> items;
        size_t vertices = 0, triangles = 0;
        double projected = 0.0;
        const double focal = double(height) / (2.0 * std::tan(glm::radians(double(lens.mFovYDegrees)) * 0.5));
        for (const Unit *unit : units)
        {
            WorldDrawItem item;
            item.mesh = unit->mesh;
            item.pose = WorldPose{*WorldPosition::FromDoubleMeters(placement + unit->origin)};
            item.tint = {0.62f, 0.76f, 0.52f, 1.0f};
            item.translationGrid = 1.0f / 4096;
            items.push_back(item);
            vertices += unit->vertices;
            triangles += unit->triangles;
            const glm::dvec3 nearest(std::clamp(view.eye.x, unit->rect.minX, unit->rect.maxX),
                                     std::clamp(view.eye.y, double(unit->minHeight), double(unit->maxHeight)),
                                     std::clamp(view.eye.z, unit->rect.minZ, unit->rect.maxZ));
            projected = std::max(projected, unit->error * focal / std::max(1.0, glm::length(nearest - view.eye)));
        }
        std::vector<DrawItem> drawItems;
        Require(BuildViewDrawItems(items, eye, render.viewProj, drawItems) == 0, "camera narrowing skipped terrain");
        render.items = drawItems;
        const std::filesystem::path png = out / (name + ".png");
        window.PollEvents();
        renderer.CaptureNextFrame(png, true);
        if (trace)
            renderer.TraceFrame({&render, 1});
        else
            renderer.DrawFrame({&render, 1});
        for (const auto &entry : LogHistory::Snapshot())
            Require(entry.level < LogLevel::Error, "renderer logged an error: " + entry.message);

        Capture capture;
        result = {{"name", name}, {"capture", png.filename().string()}, {"draws", units.size()},
                  {"vertices", vertices}, {"triangles", triangles}, {"maxProjectedErrorPixels", projected},
                  {"path", trace ? "rt" : "raster"}};
        if (trace)
        {
            std::filesystem::remove(png.string() + ".depth.f32");
            return capture;
        }
        const std::vector<float> depth = Floats(png.string() + ".depth.f32");
        Require(depth.size() == size_t(width) * height, "readback dimensions differ");
        const glm::dmat4 inverse = glm::inverse(glm::dmat4(render.viewProj));
        capture.distance.resize(depth.size(), 0.0);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = size_t(y) * width + x;
                if (depth[i] <= 0.0f)
                    continue;
                const glm::dvec4 q = inverse * glm::dvec4((double(x) + 0.5) / width * 2 - 1,
                                                         (double(y) + 0.5) / height * 2 - 1, depth[i], 1);
                capture.distance[i] = glm::length(glm::dvec3(q) / q.w);
                ++capture.hits;
            }
        std::filesystem::remove(png.string() + ".depth.f32");
        result["hitPixels"] = capture.hits;
        result["backgroundPixels"] = depth.size() - capture.hits;
        if (view.covered)
            Require(capture.hits == depth.size(), name + ": terrain leaves a hole or crack");
        if (reference)
        {
            std::vector<double> differences, relative;
            size_t silhouette = 0;
            for (size_t i = 0; i < depth.size(); ++i)
            {
                const double a = capture.distance[i], b = reference->distance[i];
                if ((a > 0.0) != (b > 0.0))
                    ++silhouette;
                else if (a > 0.0)
                {
                    differences.push_back(std::abs(a - b));
                    relative.push_back(std::abs(a - b) / b);
                }
            }
            result["silhouetteChangedPixels"] = silhouette;
            result["distanceDifferenceP50"] = Percentile(differences, 0.5);
            result["distanceDifferenceP95"] = Percentile(differences, 0.95);
            result["distanceDifferenceMax"] = Percentile(differences, 1.0);
            result["relativeDifferenceP95"] = Percentile(relative, 0.95);
        }
        return capture;
    };

    Json summary = {{"recipe", TERRAIN_REGION_RECIPE},
                    {"tiles", finalTiles.size()},
                    {"bakeSeconds", bakeSeconds},
                    {"width", width},
                    {"height", height},
                    {"placement", {placement.x, placement.y, placement.z}},
                    {"views", Json::array()},
                    {"captures", Json::array()}};
    for (const View &view : views)
        summary["views"].push_back({{"name", view.name},
                                    {"eye", {view.eye.x, view.eye.y, view.eye.z}},
                                    {"target", {view.target.x, view.target.y, view.target.z}},
                                    {"heightAboveGround", view.eye.y - ground(view.eye.x, view.eye.z)}});

    std::vector<const Unit *> tileUnits;
    for (const auto &[key, unit] : tiles)
        tileUnits.push_back(&unit);
    std::vector<Capture> references;
    for (const View &view : views)
    {
        Json result;
        references.push_back(draw(tileUnits, view, view.name + "-tiles", false, nullptr, result));
        summary["captures"].push_back(result);
        std::cout << "Captured " << view.name << " tiles\n";
    }

    for (const uint32_t regionTiles : {16u, 32u})
        for (const uint32_t cells : {64u, 128u, 256u})
        {
            std::string error;
            const std::optional<TerrainRegionBake> bake =
                BakeTerrainRegions(recipe, finalTiles, {regionTiles, cells}, error);
            Require(bake.has_value(), error);
            const TerrainRegionGrid &grid = *bake->writes.grid;
            if (regionTiles == kTerrainRegionTiles && cells == kTerrainRegionCells)
                for (size_t i = 0; i < bake->meshes.size(); ++i)
                {
                    const auto read = handle.ReadRegion(bake->meshes[i].region);
                    Require(bool(read) && read.value->positions == bake->meshes[i].positions,
                            "the release's Regions differ from the probe's bake");
                }
            std::vector<Unit> regions;
            for (const TerrainRegionMesh &mesh : bake->meshes)
            {
                Unit unit = Upload(renderer, mesh.origin.Meters(), mesh.positions, mesh.normals, mesh.indices);
                unit.rect = grid.ValidExtent(mesh.region);
                unit.error = mesh.maxDeviation;
                regions.push_back(unit);
            }
            const std::string tag =
                std::format("L0-{}m-{}cells", int(recipe.layout.TileSize() * regionTiles), cells);
            for (size_t v = 0; v < views.size(); ++v)
            {
                const View &view = views[v];
                for (uint32_t level = 0; level < grid.Levels(); ++level)
                {
                    std::vector<const Unit *> units;
                    for (size_t i = 0; i < bake->meshes.size(); ++i)
                        if (bake->meshes[i].region.level == level)
                            units.push_back(&regions[i]);
                    Json result;
                    draw(units, view, std::format("{}-{}-level{}", view.name, tag, level), false, &references[v], result);
                    result["level"] = level;
                    result["regionTiles"] = regionTiles;
                    result["cells"] = cells;
                    summary["captures"].push_back(result);
                }
                // Whole Regions near the eye give way to their children, and level zero to its tiles.
                std::vector<const Unit *> mixed;
                std::function<void(TerrainRegionCoord)> refine = [&](TerrainRegionCoord region) {
                    const TerrainLocalBounds rect = grid.ValidExtent(region);
                    const double dx = std::max({rect.minX - view.eye.x, 0.0, view.eye.x - rect.maxX});
                    const double dz = std::max({rect.minZ - view.eye.z, 0.0, view.eye.z - rect.maxZ});
                    const double edge = grid.Reference().TileSize() * double(grid.EdgeTiles(region.level));
                    if (std::hypot(dx, dz) >= edge)
                    {
                        mixed.push_back(&regions[grid.Ordinal(region)]);
                        return;
                    }
                    if (region.level > 0)
                    {
                        for (const TerrainRegionCoord child : grid.Children(region))
                            refine(child);
                        return;
                    }
                    const TileRange range = grid.MapTiles(region);
                    for (int32_t z = range.min.z; z <= range.max.z; ++z)
                        for (int32_t x = range.min.x; x <= range.max.x; ++x)
                            mixed.push_back(&tiles.at({x, z}));
                };
                refine({grid.Levels() - 1, 0, 0});
                Json result;
                draw(mixed, view, std::format("{}-{}-mixed", view.name, tag), false, &references[v], result);
                result["level"] = "mixed";
                result["regionTiles"] = regionTiles;
                result["cells"] = cells;
                summary["captures"].push_back(result);
                if (v == 0 && regionTiles == kTerrainRegionTiles && cells == kTerrainRegionCells)
                {
                    Json traced;
                    draw(mixed, view, std::format("{}-{}-mixed-rt", view.name, tag), true, nullptr, traced);
                    summary["captures"].push_back(traced);
                }
                Require(WriteText(out / "regions-probe.json", summary.dump(2) + "\n"), "cannot save the probe report");
                std::cout << "Captured " << view.name << " " << tag << '\n';
            }
            for (const Unit &unit : regions)
                renderer.DestroyMesh(unit.mesh);
        }
    Require(WriteText(out / "regions-probe.json", summary.dump(2) + "\n"), "cannot save the probe report");
    renderer.ReleaseSceneResources();
    return summary;
}
