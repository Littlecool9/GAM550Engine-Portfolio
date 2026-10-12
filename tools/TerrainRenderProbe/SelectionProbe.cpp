/**
 * @file SelectionProbe.cpp
 * @author Jackey Cen
 * @brief Captures per-viewport tile and Region selections while they stream, from several views and two cameras.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SelectionProbe.h"

#include <TerrainBake.h>
#include <TerrainFinalSurface.h>

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <core/log/LogHistory.h>
#include <core/math/WorldReal.h>
#include <renderer/MeshStreaming.h>
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainRenderMesh.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TerrainViewSelection.h>
#include <terrain/TileAddressing.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace mir;
    using namespace mir::terrainbake;
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::duration<double> kFrame{1.0 / 60.0};

    void Require(bool value, const std::string &message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    double Seconds(Clock::time_point since)
    {
        return std::chrono::duration<double>(Clock::now() - since).count();
    }

    // App's TerrainGpuUploader, over the probe's renderer.
    class ProbeUploader final : public TerrainMeshUploader
    {
    public:
        explicit ProbeUploader(VulkanRenderer &renderer) : mRenderer(renderer) {}

        MeshUploadOptions options;

        uint64_t Footprint(uint32_t vertices, uint32_t indices) const override
        {
            return mRenderer.MeshFootprint(vertices, indices, options);
        }

        uint64_t QueuedBytes() const override { return mRenderer.MeshStreaming().pendingBytes; }

        TerrainGpuMeshId Upload(TerrainMeshData &&mesh) override
        {
            std::vector<Vertex> vertices;
            vertices.reserve(mesh.vertices.size());
            for (const TerrainMeshVertex &v : mesh.vertices)
                vertices.push_back({.pos = v.position, .color = glm::vec3(1), .normal = v.normal});
            const MeshHandle handle = mRenderer.QueueMesh(std::move(vertices), std::move(mesh.indices), options);
            return handle.IsNull() ? 0 : (uint64_t(handle.mGeneration) << 32) | handle.mIndex;
        }

        TerrainGpuMeshState State(TerrainGpuMeshId mesh) const override
        {
            switch (mRenderer.MeshState(Handle(mesh)))
            {
            case MeshUploadState::Ready: return TerrainGpuMeshState::Ready;
            case MeshUploadState::Pending:
            case MeshUploadState::Uploading: return TerrainGpuMeshState::Uploading;
            default: return TerrainGpuMeshState::Failed;
            }
        }

        void Retire(TerrainGpuMeshId mesh) override { mRenderer.RetireMesh(Handle(mesh)); }
        uint64_t RetiringBytes() const override { return mRenderer.MeshStreaming().retiringBytes; }

        static MeshHandle Handle(TerrainGpuMeshId mesh)
        {
            return MeshHandle{uint32_t(mesh & 0xffffffffu), uint32_t(mesh >> 32)};
        }

    private:
        VulkanRenderer &mRenderer;
    };

    struct WireMesh
    {
        MeshHandle handle;
        size_t vertices = 0;
    };

    // A camera in terrain-local meters.
    struct Eye
    {
        std::string name;
        glm::dvec3 eye{0.0};
        glm::dvec3 target{0.0};
        double fovY = 60.0;
        double rollDegrees = 0.0;
        // Zero: the whole frame.
        VkRect2D rect{};
    };

    struct Slot
    {
        std::unique_ptr<TerrainViewSelector> selector;
        TerrainInterestId interest = 0;
    };

    enum class Look
    {
        Lit,
        Detail,
        Wire,
    };

    // Tiles green, darker per coarser LOD; Regions blue to violet per level.
    glm::vec4 DetailTint(const TerrainDrawUnit &unit)
    {
        const float level = float(unit.geometry.level);
        if (unit.kind == TerrainUnitKind::Tile)
            return {0.35f, std::max(0.35f, 0.9f - 0.18f * level), 0.3f, 1.0f};
        return {std::min(0.9f, 0.3f + 0.25f * level), 0.35f, 0.95f, 1.0f};
    }

    Json Levels(std::span<const TerrainDrawUnit> draws)
    {
        std::map<std::string, size_t> counts;
        for (const TerrainDrawUnit &unit : draws)
            ++counts[std::format("{} L{}", unit.kind == TerrainUnitKind::Tile ? "tile" : "Region", unit.geometry.level)];
        Json levels = Json::object();
        for (const auto &[name, count] : counts)
            levels[name] = count;
        return levels;
    }

    // Map tiles covered by more than one drawn unit.
    size_t Overlaps(const TerrainIndex &index, std::span<const TerrainDrawUnit> draws)
    {
        std::unordered_map<TileCoord, int> covered;
        for (const TerrainDrawUnit &unit : draws)
        {
            if (unit.kind == TerrainUnitKind::Tile)
            {
                ++covered[unit.geometry.coord];
                continue;
            }
            const TileRange range =
                index.regionGrid->MapTiles({unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                    ++covered[{x, z}];
        }
        return size_t(std::count_if(covered.begin(), covered.end(), [](const auto &entry) { return entry.second > 1; }));
    }
}

nlohmann::json RunSelectionProbe(VulkanRenderer &renderer, Window &window, const std::filesystem::path &out,
                                 uint32_t width, uint32_t height)
{
    std::filesystem::create_directories(out);
    const RecipeParseResult parsed = LoadTerrainRecipe(TERRAIN_REGION_RECIPE);
    Require(parsed.recipe.has_value(), "selection probe recipe cannot load");
    const TerrainRecipe &recipe = *parsed.recipe;

    const auto bakeStart = Clock::now();
    TerrainReport report("bake", {check::kGenerate, check::kEncode});
    std::optional<std::vector<TileHeights>> heights = BakeFinalHeights(recipe, report);
    Require(heights.has_value(), "selection probe final heights failed");
    const std::vector<TileHeights> finalTiles = *heights;
    std::optional<TerrainBakedRelease> baked = EncodeBakedRelease(
        MakeAssetId("probe/selection.terrain.json"), FingerprintRecipe(recipe), recipe, std::move(*heights),
        {125, {}, {}}, report);
    if (!baked)
        for (const auto &d : report.diagnostics)
            std::cerr << FormatDiagnostic(d) << '\n';
    Require(baked.has_value(), "selection probe bake failed");
    const double bakeSeconds = Seconds(bakeStart);
    std::cout << "Baked the selection probe map in " << bakeSeconds << " s\n";

    TerrainFinalSurface final{recipe, {}};
    for (const TileHeights &tile : finalTiles)
        final.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile);
    const auto ground = [&](double x, double z)
    {
        const TileAddress a = AddressLocal(recipe.layout, *TerrainLocalPosition::FromMeters({x, 0.0, z}));
        return a.IsInside() ? final.SharedHeight(x, z, *final.Tile(a.tile)) : 0.0;
    };

    // TerrainCheck's view settings over this map by interest, first with room for everything it wants.
    const TerrainStreamingSettingsLoad load = LoadTerrainStreamingSettings(TERRAIN_CHECK_STREAMING, {});
    Require(load.settings.has_value(), "TerrainCheck streaming settings do not load");
    TerrainStreamingSettings settings = *load.settings;
    const uint64_t roomyBudget = uint64_t(512) << 20, tightBudget = uint64_t(64) << 20;
    settings.residency.mode = TerrainResidencyMode::Interest;
    auto opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(baked->encoded));
    Require(bool(opened), opened.error.message);
    const std::shared_ptr<TerrainResidentRelease> release = TerrainResidentRelease::Create(std::move(*opened.value));
    const TerrainIndex &index = release->Handle().Index();
    std::unique_ptr<TerrainStreamer> streamer;

    const glm::dvec3 placementMeters = kLargeWorld ? glm::dvec3(150000.5, -20.0, -75000.25) : glm::dvec3(0.0, -20.0, 0.0);
    const TerrainPlacement placement{release->Info().asset, *WorldPosition::FromDoubleMeters(placementMeters)};
    const TerrainSelectionInstance instance{release->SharedHandle(), placement};

    renderer.SetDirectionalLight({-0.35f, 0.8f, -0.25f}, {0.95f, 0.9f, 0.8f});
    renderer.SetAmbientColor({0.25f, 0.28f, 0.32f});
    renderer.SetFog({0.62f, 0.7f, 0.8f}, 0.00004f);
    MaterialDesc wireDesc;
    wireDesc.shaderName = "terrainwire";
    const MaterialHandle wireMaterial = renderer.CreateMaterial(wireDesc);
    Require(!wireMaterial.IsNull(), "wire material failed");

    Require(renderer.ConfigureMeshStreaming({.stagingBytes = settings.gpu.stagingBytes,
                                             .frameBytes = settings.gpu.uploadBytesPerFrame}),
            "mesh streaming does not configure");
    const uint64_t roomyGpuBudget = uint64_t(1024) << 20;
    ProbeUploader uploader(renderer);
    std::shared_ptr<TerrainWorkerPool> pool;
    std::unique_ptr<TerrainMeshResidency> residency;
    const auto probeStart = Clock::now();
    const auto now = [&] { return Seconds(probeStart); };
    const TerrainUnitReady ready = [&](const TerrainDrawUnit &unit) { return residency->Ready(unit.geometry); };

    // Captures only: the wireframe, built and uploaded at once.
    std::unordered_map<TerrainPayloadKey, WireMesh> wires;
    const auto wireOf = [&](const TerrainDrawUnit &unit) -> MeshHandle
    {
        if (const auto found = wires.find(unit.geometry); found != wires.end())
            return found->second.handle;
        std::vector<TerrainMeshVertex> vertices;
        std::vector<uint32_t> indices;
        if (unit.kind == TerrainUnitKind::Region)
        {
            const auto region = streamer->Region(unit.geometry);
            Require(region != nullptr, "a drawn Region is not resident");
            for (size_t i = 0; i < region->positions.size(); ++i)
                vertices.push_back({region->positions[i], region->normals[i]});
            indices = region->indices;
        }
        else
        {
            const auto lod = streamer->Lod(unit.geometry);
            const auto normals = streamer->Normals(unit.normals);
            Require(lod && normals, "a drawn tile is not resident");
            auto built = MakeTerrainRenderMesh(index, *lod, *normals);
            Require(bool(built), built.error.message);
            vertices = built.value->vertices;
            indices = built.value->indices;
        }
        std::vector<Vertex> expanded;
        std::vector<uint32_t> sequence;
        for (size_t i = 0; i < indices.size(); ++i)
        {
            const TerrainMeshVertex &source = vertices[indices[i]];
            Vertex v{.pos = source.position, .color = glm::vec3(0), .normal = source.normal};
            v.color[int(i % 3)] = 1;
            sequence.push_back(uint32_t(expanded.size()));
            expanded.push_back(v);
        }
        const MeshHandle wire = renderer.CreateMesh(expanded, sequence);
        Require(!wire.IsNull(), "wire upload failed");
        wires[unit.geometry] = {wire, expanded.size()};
        return wire;
    };

    std::vector<Slot> slots;
    const auto slot = [&](size_t i) -> Slot &
    {
        while (slots.size() <= i)
            slots.push_back({std::make_unique<TerrainViewSelector>(settings.view), streamer->AddInterest()});
        return slots[i];
    };
    std::vector<std::vector<TerrainDrawUnit>> previous;
    // Nothing resident or uploaded, under new budgets and with or without a BLAS per mesh.
    const auto restart = [&](uint64_t budgetBytes, uint64_t gpuBudgetBytes, bool rayTracing)
    {
        if (residency)
            residency->RetireAll();
        std::vector<MeshHandle> handles;
        for (const auto &[key, wire] : wires)
            handles.push_back(wire.handle);
        if (!handles.empty())
            renderer.ReleaseModelResources(handles, {}, {});
        // Empty frames until the retired meshes are freed, so a flight starts with nothing held.
        for (int i = 0; i < 4; ++i)
        {
            window.PollEvents();
            renderer.DrawFrame({});
        }
        wires.clear();
        slots.clear();
        previous.clear();
        residency.reset();
        streamer.reset();
        settings.cpu.budgetBytes = budgetBytes;
        settings.gpu.budgetBytes = gpuBudgetBytes;
        pool = std::make_shared<TerrainWorkerPool>(settings.io.workerThreads);
        streamer = std::make_unique<TerrainStreamer>(settings, pool);
        Require(streamer->Add(release) == TerrainResidencyMode::Interest, "the probe map must stream by interest");
        uploader.options.rayTracing = rayTracing;
        residency = std::make_unique<TerrainMeshResidency>(settings.gpu, pool, uploader);
    };
    restart(roomyBudget, roomyGpuBudget, false);

    struct FrameResult
    {
        Json views = Json::array();
        bool settled = true;
        size_t background = 0;
        size_t hits = 0;
    };
    const auto vertexCount = [&](const TerrainDrawUnit &unit) -> size_t
    {
        const TerrainTileRecord *record =
            unit.kind == TerrainUnitKind::Region
                ? index.FindRegion({unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z})
                : index.Find(unit.geometry.coord, unit.geometry.layer, TerrainPayloadKind::LodHeight, unit.geometry.level);
        return record ? record->vertexCount : 0;
    };

    // One frame: streamer and mesh installs, per-view selection and wants, mesh requests, draw.
    const auto frame = [&](const std::vector<Eye> &eyes, Look look, const std::optional<std::filesystem::path> &capture,
                           bool trace) -> FrameResult
    {
        const auto frameStart = Clock::now();
        streamer->Update();
        residency->Install();
        const double installSeconds = Seconds(frameStart);
        const MeshStreamingStats uploadsBefore = renderer.MeshStreaming();
        const TerrainMeshStats meshesBefore = residency->Stats();
        FrameResult result;
        std::vector<std::vector<DrawItem>> items(eyes.size());
        std::vector<RenderView> views;
        previous.resize(std::max(previous.size(), eyes.size()));
        struct Pass
        {
            WorldPosition eye;
            glm::mat4 viewProj{1.0f};
            Camera lens;
        };
        std::vector<Pass> passes;
        std::vector<TerrainPayloadWant> wants;
        double selectSeconds = 0.0;
        for (size_t i = 0; i < eyes.size(); ++i)
        {
            const Eye &e = eyes[i];
            const VkExtent2D extent = e.rect.extent.width ? e.rect.extent : VkExtent2D{width, height};
            const WorldPosition eye = *WorldPosition::FromDoubleMeters(placementMeters + e.eye);
            const glm::vec3 forward = glm::normalize(glm::vec3(e.target - e.eye));
            const float roll = float(glm::radians(e.rollDegrees));
            const glm::vec3 up = glm::vec3(0, 1, 0) * std::cos(roll) + glm::cross(forward, glm::vec3(0, 1, 0)) * std::sin(roll) +
                                 forward * glm::dot(forward, glm::vec3(0, 1, 0)) * (1.0f - std::cos(roll));
            const glm::mat4 orientation = glm::inverse(glm::lookAt(glm::vec3(0), forward, up));
            Camera lens;
            lens.mFovYDegrees = float(e.fovY);
            lens.mNearZ = 0.5f;
            lens.mFarZ = 0.0f;
            const glm::mat4 viewProj =
                MakeCameraRelativeViewProjection({eye, glm::mat3(orientation)}, lens, float(extent.width) / extent.height);

            Slot &s = slot(i);
            TerrainView view;
            view.frustum = {eye, viewProj};
            view.focalPixels = double(extent.height) / (2.0 * std::tan(glm::radians(e.fovY) * 0.5));
            const auto selectStart = Clock::now();
            s.selector->Update({&instance, 1}, view, ready);
            streamer->SetInterest(s.interest, s.selector->Wants());
            wants.insert(wants.end(), s.selector->Wants().begin(), s.selector->Wants().end());
            selectSeconds += Seconds(selectStart);
            passes.push_back({eye, viewProj, lens});
        }
        residency->Request({&instance, 1}, wants, *streamer, now());
        const double requestSeconds = residency->Stats().lastRequestSeconds;

        for (size_t i = 0; i < eyes.size(); ++i)
        {
            const Eye &e = eyes[i];
            Slot &s = slots[i];
            const WorldPosition eye = passes[i].eye;
            const glm::mat4 viewProj = passes[i].viewProj;
            const Camera lens = passes[i].lens;
            std::vector<WorldDrawItem> world;
            size_t vertices = 0;
            double worstError = 0.0;
            for (const TerrainDrawUnit &unit : s.selector->Draws())
            {
                const TerrainResidentMesh *mesh = residency->Acquire(unit.geometry, now());
                Require(mesh != nullptr, "a ready unit has no mesh");
                WorldDrawItem draw;
                draw.mesh = look == Look::Wire ? wireOf(unit) : ProbeUploader::Handle(mesh->id);
                draw.material = look == Look::Wire ? wireMaterial : kNullMaterial;
                draw.pose = WorldPose{*TryTerrainLocalToWorld(mesh->origin, placement.translation)};
                draw.tint = look == Look::Detail ? DetailTint(unit) : glm::vec4(0.62f, 0.76f, 0.52f, 1.0f);
                draw.translationGrid = 1.0f / 4096;
                world.push_back(draw);
                vertices += vertexCount(unit);
                if (unit.kind == TerrainUnitKind::Region || unit.geometry.level > 0)
                    worstError = std::max(worstError, unit.errorPixels);
            }
            Require(BuildViewDrawItems(world, eye, viewProj, items[i]) == 0, "camera narrowing skipped terrain");
            views.push_back(RenderView{e.rect, items[i], viewProj, eye, lens.mNearZ, 0.0f});

            const std::span<const TerrainDrawUnit> draws = s.selector->Draws();
            std::unordered_set<TerrainPayloadKey> before;
            for (const TerrainDrawUnit &unit : previous[i])
                before.insert(unit.geometry);
            size_t changed = 0;
            for (const TerrainDrawUnit &unit : draws)
                changed += before.erase(unit.geometry) == 0 ? 1 : 0;
            changed += before.size();
            previous[i].assign(draws.begin(), draws.end());

            const TerrainViewStats &stats = s.selector->Stats();
            size_t visibleNotReady = 0;
            for (const TerrainPayloadWant &want : s.selector->Wants())
                if (want.demand == TerrainDemand::Visible &&
                    streamer->Status(want.key).state != TerrainPayloadState::Ready)
                    ++visibleNotReady;
            const size_t overlaps = Overlaps(index, draws);
            Require(overlaps == 0, e.name + ": a map tile is drawn twice");
            result.settled = result.settled && stats.missing == 0 && stats.coarserFallbacks == 0 &&
                             stats.otherFallbacks == 0 && visibleNotReady == 0;
            result.views.push_back({{"name", e.name},
                                    {"heightAboveGround", e.eye.y - ground(e.eye.x, e.eye.z)},
                                    {"fovY", e.fovY},
                                    {"draws", draws.size()},
                                    {"levels", Levels(draws)},
                                    {"vertices", vertices},
                                    {"maxProjectedErrorPixels", worstError},
                                    {"missing", stats.missing},
                                    {"coarserStandIns", stats.coarserFallbacks},
                                    {"otherStandIns", stats.otherFallbacks},
                                    {"visibleWants", stats.visibleWants},
                                    {"visibleNotReady", visibleNotReady},
                                    {"prefetchWants", stats.prefetchWants},
                                    {"prefetchMiB", double(stats.prefetchBytes) / (1 << 20)},
                                    {"prefetchOverBudget", stats.prefetchDropped},
                                    {"culled", stats.culled},
                                    {"unitsChanged", changed},
                                    {"overlappingTiles", overlaps}});
        }
        const TerrainStreamingStats stream = streamer->Stats();
        window.PollEvents();
        if (capture)
            renderer.CaptureNextFrame(*capture, !trace);
        if (trace)
            renderer.TraceFrame(views);
        else
            renderer.DrawFrame(views);
        const MeshStreamingStats uploads = renderer.MeshStreaming();
        const TerrainMeshStats meshes = residency->Stats();
        const auto mib = [](uint64_t bytes) { return double(bytes) / (1 << 20); };
        for (Json &view : result.views)
        {
            view["meshesBuilt"] = meshes.buildsStarted - meshesBefore.buildsStarted;
            view["residentMiB"] = mib(stream.residentBytes);
            view["loading"] = stream.loading;
            view["readsStarted"] = stream.readsStarted;
            view["evicted"] = stream.evicted;
            view["installMs"] = installSeconds * 1000.0;
            view["selectMs"] = selectSeconds * 1000.0;
            view["requestMs"] = requestSeconds * 1000.0;
            view["uploadMiB"] = mib(uploads.lastFrameBytes);
            view["gpuHeldMiB"] = mib(meshes.heldBytes);
            view["gpuPendingMiB"] = mib(uploads.pendingBytes);
            view["gpuUploadingMiB"] = mib(uploads.uploadingBytes);
            view["gpuRetiringMiB"] = mib(uploads.retiringBytes);
            view["meshesReady"] = meshes.ready;
            view["meshesInFlight"] = meshes.building + meshes.built + meshes.uploading;
            view["meshesRetired"] = meshes.retired - meshesBefore.retired;
            view["deviceWaits"] = uploads.deviceWaits - uploadsBefore.deviceWaits;
            view["queueWaits"] = uploads.queueWaits - uploadsBefore.queueWaits;
            view["skippedDraws"] = uploads.skippedDraws - uploadsBefore.skippedDraws;
            view["highestGeneration"] = uploads.highestGeneration;
            view["blasBuilt"] = uploads.blasBuilt - uploadsBefore.blasBuilt;
            view["frameMs"] = Seconds(frameStart) * 1000.0;
        }
        for (const auto &entry : LogHistory::Snapshot())
            Require(entry.level < LogLevel::Error, "renderer logged an error: " + entry.message);
        if (capture && !trace)
        {
            const auto bytes = ReadBytes(capture->string() + ".depth.f32");
            Require(bytes && bytes->size() == size_t(width) * height * sizeof(float), "missing depth readback");
            std::vector<float> depth(bytes->size() / sizeof(float));
            std::memcpy(depth.data(), bytes->data(), bytes->size());
            for (float d : depth)
                ++(d > 0.0f ? result.hits : result.background);
            std::filesystem::remove(capture->string() + ".depth.f32");
        }
        return result;
    };

    // Frames at a steady rate until every view draws what it chose, with nothing standing in.
    const auto settle = [&](const std::vector<Eye> &eyes) -> Json
    {
        const auto start = Clock::now();
        for (int i = 0; i < 1200; ++i)
        {
            const auto next = Clock::now() + kFrame;
            if (frame(eyes, Look::Lit, std::nullopt, false).settled)
                return {{"frames", i + 1}, {"seconds", Seconds(start)}};
            std::this_thread::sleep_until(next);
        }
        throw std::runtime_error("views did not settle: " + eyes.front().name);
    };

    const TerrainLocalBounds bounds = recipe.layout.Bounds();
    const double cx = (bounds.minX + bounds.maxX) * 0.5, cz = (bounds.minZ + bounds.maxZ) * 0.5;
    const auto at = [&](double x, double above, double z) { return glm::dvec3(x, ground(x, z) + above, z); };
    const std::vector<Eye> eyes{
        {"high-altitude", at(cx - 3000, 3000, cz - 2500), at(cx + 4000, 0, cz + 3000)},
        {"low-approach", at(cx - 5200, 90, cz + 300), at(cx - 2500, 0, cz + 300)},
        {"tilted", at(cx + 300, 400, cz - 700), at(cx - 700, 0, cz + 900), 50.0, 35.0},
        {"map-edge", at(bounds.minX + 150, 60, cz - 1500), at(bounds.minX - 400, 0, cz + 1500)},
        {"nadir", at(cx, 3500, cz), at(cx, 0, cz + 1e-3)}};

    Json summary = {{"recipe", TERRAIN_REGION_RECIPE},
                    {"settings", TERRAIN_CHECK_STREAMING},
                    {"view",
                     {{"refineErrorPixels", settings.view.refineErrorPixels},
                      {"mergeErrorPixels", settings.view.mergeErrorPixels},
                      {"prefetchErrorPixels", settings.view.prefetchErrorPixels},
                      {"prefetchBudgetMiB", double(*settings.view.prefetchBudgetBytes) / (1 << 20)}}},
                    {"cpuBudgetMiB", double(settings.cpu.budgetBytes) / (1 << 20)},
                    {"bakeSeconds", bakeSeconds},
                    {"width", width},
                    {"height", height},
                    {"placement", {placementMeters.x, placementMeters.y, placementMeters.z}},
                    {"captures", Json::array()}};
    const auto save = [&] { Require(WriteText(out / "selection-probe.json", summary.dump(2) + "\n"), "cannot save"); };

    for (const Eye &e : eyes)
    {
        Json settled = settle({e});
        for (const Look look : {Look::Lit, Look::Detail, Look::Wire})
        {
            const char *suffix = look == Look::Lit ? "lit" : look == Look::Detail ? "detail" : "wire";
            const std::filesystem::path png = out / std::format("{}-{}.png", e.name, suffix);
            FrameResult result = frame({e}, look, png, false);
            Json capture = result.views[0];
            capture["capture"] = png.filename().string();
            capture["settle"] = settled;
            capture["hitPixels"] = result.hits;
            capture["backgroundPixels"] = result.background;
            summary["captures"].push_back(capture);
            if (e.name == "nadir")
                Require(result.background == 0, "nadir: terrain leaves a hole or crack");
        }
        std::cout << "Captured " << e.name << '\n';
        save();
    }

    {
        restart(roomyBudget, roomyGpuBudget, true);
        Json settled = settle({eyes[0]});
        const std::filesystem::path png = out / "high-altitude-rt.png";
        FrameResult result = frame({eyes[0]}, Look::Lit, png, true);
        Json capture = result.views[0];
        capture["capture"] = png.filename().string();
        capture["path"] = "rt";
        capture["settle"] = settled;
        summary["captures"].push_back(capture);
        restart(roomyBudget, roomyGpuBudget, false);
    }

    {
        const glm::dvec3 focus = at(cx - 1200, 0, cz + 800);
        Eye wide{"two-cameras-wide", at(cx - 1700, 120, cz + 300), focus, 90.0, 0.0,
                 {{0, 0}, {width / 2, height}}};
        Eye narrow{"two-cameras-narrow", at(cx - 6000, 1500, cz - 2500), focus, 12.0, 0.0,
                   {{int32_t(width / 2), 0}, {width - width / 2, height}}};
        Json settled = settle({wide, narrow});
        const std::filesystem::path png = out / "two-cameras-detail.png";
        FrameResult result = frame({wide, narrow}, Look::Detail, png, false);
        // What draws each map tile in a view: a tile at a LOD, or a Region of a level.
        const auto representation = [&](const TerrainViewSelector &selector)
        {
            std::unordered_map<TileCoord, std::pair<TerrainUnitKind, uint32_t>> drawn;
            for (const TerrainDrawUnit &unit : selector.Draws())
            {
                if (unit.kind == TerrainUnitKind::Tile)
                {
                    drawn[unit.geometry.coord] = {unit.kind, unit.geometry.level};
                    continue;
                }
                const TileRange range =
                    index.regionGrid->MapTiles({unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
                for (int32_t z = range.min.z; z <= range.max.z; ++z)
                    for (int32_t x = range.min.x; x <= range.max.x; ++x)
                        drawn[{x, z}] = {unit.kind, unit.geometry.level};
            }
            return drawn;
        };
        const auto wideDrawn = representation(*slots[0].selector);
        size_t sameArea = 0, differentDetail = 0;
        for (const auto &[tile, form] : representation(*slots[1].selector))
            if (const auto found = wideDrawn.find(tile); found != wideDrawn.end())
            {
                ++sameArea;
                differentDetail += found->second != form ? 1 : 0;
            }
        std::unordered_set<TerrainPayloadKey> wideWants;
        for (const TerrainPayloadWant &want : slots[0].selector->Wants())
            wideWants.insert(want.key);
        const size_t shared = size_t(std::count_if(slots[1].selector->Wants().begin(), slots[1].selector->Wants().end(),
                                                   [&](const TerrainPayloadWant &w) { return wideWants.contains(w.key); }));
        summary["twoCameras"] = {{"capture", png.filename().string()},
                                 {"settle", settled},
                                 {"views", result.views},
                                 {"mapTilesDrawnByBoth", sameArea},
                                 {"mapTilesAtDifferentDetail", differentDetail},
                                 {"sharedPayloadWants", shared},
                                 {"readsStarted", streamer->Stats().readsStarted}};
        std::cout << "Captured two cameras\n";
        save();
    }

    const auto percentile = [](std::vector<double> values, double p)
    {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[std::min(values.size() - 1, size_t(p * double(values.size() - 1) + 0.5))];
    };
    const auto spread = [&](const std::vector<double> &values)
    {
        return Json{{"p50", percentile(values, 0.5)}, {"p95", percentile(values, 0.95)},
                    {"p99", percentile(values, 0.99)}, {"max", percentile(values, 1.0)}};
    };

    // Frames at a steady rate along a path, starting with nothing resident. event runs before its frame; after
    // each mark, the frames until nothing is missing or standing in are counted.
    struct Flight
    {
        std::string name;
        int frames = 0;
        std::function<Eye(int)> path;
        uint64_t cpuBudget = 0;
        uint64_t gpuBudget = 0;
        bool rayTracing = false;
        std::function<void(int)> event;
        std::vector<int> marks;
        // TerrainCheck's otherwise.
        std::optional<uint64_t> prefetchBudget;
    };
    const std::optional<uint64_t> checkPrefetchBudget = settings.view.prefetchBudgetBytes;
    const auto fly = [&](const Flight &flight)
    {
        settings.view.prefetchBudgetBytes = flight.prefetchBudget ? flight.prefetchBudget : checkPrefetchBudget;
        restart(flight.cpuBudget, flight.gpuBudget, flight.rayTracing);
        const TerrainMeshStats meshesBefore = residency->Stats();
        const MeshStreamingStats uploadsBefore = renderer.MeshStreaming();
        size_t worstMissing = 0, framesWithHoles = 0, framesWithStandIns = 0, totalChanged = 0;
        uint64_t deviceWaits = 0, queueWaits = 0, skippedDraws = 0;
        double peakHeld = 0.0, peakRetiring = 0.0, maxUpload = 0.0;
        std::vector<double> frameMs, selectMs, requestMs;
        Json records = Json::array();
        Json settles = Json::array();
        int mark = -1;
        const auto begin = Clock::now();
        for (int i = 0; i <= flight.frames; ++i)
        {
            const auto next = Clock::now() + kFrame;
            if (flight.event)
                flight.event(i);
            if (std::find(flight.marks.begin(), flight.marks.end(), i) != flight.marks.end())
            {
                if (mark >= 0)
                    settles.push_back({{"from", mark}, {"frames", nullptr}});
                mark = i;
            }
            Eye e = flight.path(i);
            e.name = flight.name;
            const bool capture = !flight.rayTracing && i % 150 == 0;
            const std::filesystem::path png = out / std::format("{}-{:04}.png", flight.name, i);
            FrameResult result =
                frame({e}, Look::Detail, capture ? std::optional(png) : std::nullopt, flight.rayTracing);
            Json record = result.views[0];
            record["frame"] = i;
            record["seconds"] = Seconds(begin);
            if (capture)
            {
                record["capture"] = png.filename().string();
                record["backgroundPixels"] = result.background;
            }
            else
            {
                frameMs.push_back(record["frameMs"].get<double>());
                selectMs.push_back(record["selectMs"].get<double>());
                requestMs.push_back(record["requestMs"].get<double>());
                deviceWaits += record["deviceWaits"].get<uint64_t>();
                queueWaits += record["queueWaits"].get<uint64_t>();
            }
            skippedDraws += record["skippedDraws"].get<uint64_t>();
            peakHeld = std::max(peakHeld, record["gpuHeldMiB"].get<double>());
            peakRetiring = std::max(peakRetiring, record["gpuRetiringMiB"].get<double>());
            maxUpload = std::max(maxUpload, record["uploadMiB"].get<double>());
            const size_t missing = record["missing"].get<size_t>();
            const size_t standIns = record["coarserStandIns"].get<size_t>() + record["otherStandIns"].get<size_t>();
            worstMissing = std::max(worstMissing, missing);
            framesWithHoles += missing > 0 ? 1 : 0;
            framesWithStandIns += standIns > 0 ? 1 : 0;
            totalChanged += record["unitsChanged"].get<size_t>();
            if (mark >= 0 && missing == 0 && standIns == 0)
            {
                settles.push_back({{"from", mark}, {"frames", i - mark}});
                mark = -1;
            }
            records.push_back(record);
            std::this_thread::sleep_until(next);
        }
        if (mark >= 0)
            settles.push_back({{"from", mark}, {"frames", nullptr}});
        const TerrainStreamingStats stats = streamer->Stats();
        const TerrainMeshStats meshes = residency->Stats();
        const MeshStreamingStats uploads = renderer.MeshStreaming();
        summary[flight.name] = {{"cpuBudgetMiB", double(flight.cpuBudget) / (1 << 20)},
                                {"gpuBudgetMiB", double(flight.gpuBudget) / (1 << 20)},
                                {"prefetchBudgetMiB", double(*settings.view.prefetchBudgetBytes) / (1 << 20)},
                                {"uploadMiBPerFrame", double(settings.gpu.uploadBytesPerFrame) / (1 << 20)},
                                {"rayTracing", flight.rayTracing},
                                {"frames", flight.frames + 1},
                                {"seconds", Seconds(begin)},
                                {"framesWithMissingAreas", framesWithHoles},
                                {"worstMissingAreas", worstMissing},
                                {"framesWithStandIns", framesWithStandIns},
                                {"unitsChangedPerFrame", double(totalChanged) / (flight.frames + 1)},
                                {"meshesBuilt", meshes.buildsStarted - meshesBefore.buildsStarted},
                                {"meshesRetired", meshes.retired - meshesBefore.retired},
                                {"meshesEvicted", meshes.evicted - meshesBefore.evicted},
                                {"budgetWaits", meshes.budgetWaits - meshesBefore.budgetWaits},
                                {"maxBuildMs", meshes.maxBuildSeconds * 1000.0},
                                {"uploadedMiB", double(uploads.uploadedBytes - uploadsBefore.uploadedBytes) / (1 << 20)},
                                {"maxUploadMiBPerFrame", maxUpload},
                                {"blasBuilt", uploads.blasBuilt - uploadsBefore.blasBuilt},
                                {"peakGpuHeldMiB", peakHeld},
                                {"peakGpuRetiringMiB", peakRetiring},
                                {"deviceWaits", deviceWaits},
                                {"queueWaits", queueWaits},
                                {"skippedDraws", skippedDraws},
                                {"highestGeneration", uploads.highestGeneration},
                                {"readsStarted", stats.readsStarted},
                                {"evicted", stats.evicted},
                                {"peakMiB", double(stats.peakBytes) / (1 << 20)},
                                {"frameMs", spread(frameMs)},
                                {"selectMs", spread(selectMs)},
                                {"requestMs", spread(requestMs)},
                                {"settles", settles},
                                {"records", records}};
        std::cout << "Flew " << flight.name << '\n';
        save();
    };

    // From far out and high up down to a fine tile.
    const glm::dvec3 approachStart = at(cx - 7000, 6000, cz - 7000);
    const glm::dvec3 approachEnd = at(cx + 100, 25, cz + 100);
    const auto approach = [&](int i)
    {
        const double t = double(i) / 1800.0;
        const double s = t * t * (3.0 - 2.0 * t);
        Eye e{"", approachStart + (approachEnd - approachStart) * s, {}, 60.0};
        e.eye.y = std::max(e.eye.y, ground(e.eye.x, e.eye.z) + 25.0);
        const glm::dvec3 ahead = approachEnd + glm::dvec3(600.0, 0.0, 600.0);
        e.target = glm::dvec3(ahead.x, ground(ahead.x, ahead.z), ahead.z);
        return e;
    };
    // Low and fast in a straight line, looking ahead.
    const auto straight = [&](int i)
    {
        const double x = cx - 5000.0 + 250.0 * i / 60.0;
        return Eye{"", at(x, 300, cz - 600), at(x + 2000.0, 0, cz - 600), 60.0};
    };
    // Hovering, yawing half a turn a second, reversing every two seconds.
    const auto turns = [&](int i)
    {
        const int leg = i / 120;
        const double yaw = glm::radians((leg % 2 == 0 ? 3.0 : -3.0) * (i % 120) + (leg % 2 == 0 ? 0.0 : 360.0));
        const glm::dvec3 eye = at(cx + 800, 150, cz - 400);
        return Eye{"", eye, eye + glm::dvec3(std::cos(yaw) * 1000.0, -250.0, std::sin(yaw) * 1000.0), 60.0};
    };
    // A jump to another corner of the map every two and a half seconds.
    const std::vector<glm::dvec3> corners{at(cx - 5000, 200, cz - 5000), at(cx + 5000, 120, cz + 4500),
                                          at(cx - 4500, 400, cz + 5000), at(cx + 4800, 80, cz - 4800)};
    const auto teleport = [&](int i)
    {
        const glm::dvec3 eye = corners[size_t(i / 150) % corners.size()];
        return Eye{"", eye, at(cx, 0, cz), 60.0};
    };
    const std::vector<int> jumps{0, 150, 300, 450, 600, 750, 900, 1050};
    // Halfway, the scene goes: every GPU mesh is released at once, as a scene unload does.
    const auto unload = [&](int i)
    {
        if (i != 300)
            return;
        residency->Forget();
        wires.clear();
        renderer.ReleaseSceneResources();
    };
    const uint64_t tightGpuBudget = uint64_t(48) << 20;
    const uint64_t roomyPrefetchBudget = uint64_t(64) << 20;

    fly({"approach", 1800, approach, roomyBudget, roomyGpuBudget, false});
    fly({"approach-rt", 1800, approach, roomyBudget, roomyGpuBudget, true});
    fly({"approach-tight", 1800, approach, tightBudget, roomyGpuBudget, false});
    fly({"approach-gpu-tight", 1800, approach, roomyBudget, tightGpuBudget, false});
    fly({"straight", 1200, straight, roomyBudget, roomyGpuBudget, false});
    fly({"straight-rt", 1200, straight, roomyBudget, roomyGpuBudget, true});
    fly({"turns", 960, turns, roomyBudget, roomyGpuBudget, false, {}, {0}});
    fly({"turns-rt", 960, turns, roomyBudget, roomyGpuBudget, true, {}, {0}});
    fly({"turns-prefetch", 960, turns, roomyBudget, roomyGpuBudget, false, {}, {0}, roomyPrefetchBudget});
    fly({"teleport", 1199, teleport, roomyBudget, roomyGpuBudget, false, {}, jumps});
    fly({"teleport-rt", 1199, teleport, roomyBudget, roomyGpuBudget, true, {}, jumps});
    fly({"unload", 600, straight, roomyBudget, roomyGpuBudget, false, unload, {0, 300}});
    fly({"unload-rt", 600, straight, roomyBudget, roomyGpuBudget, true, unload, {0, 300}});

    residency->RetireAll();
    residency.reset();
    return summary;
}
