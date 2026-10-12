/**
 * @file TerrainMeshResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainMeshResidency.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainViewSelection.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Demand = TerrainDemand;

    // What the residency is told by a renderer: queued, then recorded, then complete.
    class FakeUploader final : public TerrainMeshUploader
    {
    public:
        enum class Stage
        {
            Queued,
            Recorded,
            Ready,
            Failed,
        };

        struct Mesh
        {
            TerrainPayloadKey geometry;
            uint64_t bytes = 0;
            size_t vertices = 0;
            Stage stage = Stage::Queued;
        };

        uint64_t Footprint(uint32_t vertices, uint32_t indices) const override
        {
            return uint64_t(vertices) * 64 + uint64_t(indices) * 4;
        }

        uint64_t QueuedBytes() const override
        {
            uint64_t bytes = 0;
            for (const auto &[id, mesh] : meshes)
                bytes += mesh.stage == Stage::Queued ? mesh.bytes : 0;
            return bytes;
        }

        TerrainGpuMeshId Upload(TerrainMeshData &&data) override
        {
            uploadOrder.push_back(data.geometry);
            if (refuse)
                return 0;
            const TerrainGpuMeshId id = nextId++;
            meshes[id] = {data.geometry, Footprint(uint32_t(data.vertices.size()), uint32_t(data.indices.size())),
                          data.vertices.size()};
            return id;
        }

        TerrainGpuMeshState State(TerrainGpuMeshId id) const override
        {
            switch (meshes.at(id).stage)
            {
            case Stage::Ready: return TerrainGpuMeshState::Ready;
            case Stage::Failed: return TerrainGpuMeshState::Failed;
            default: return TerrainGpuMeshState::Uploading;
            }
        }

        void Retire(TerrainGpuMeshId id) override
        {
            const auto found = meshes.find(id);
            REQUIRE(found != meshes.end());
            retired.push_back(found->second.geometry);
            if (found->second.stage != Stage::Queued)
                retiring += found->second.bytes;
            meshes.erase(found);
        }

        uint64_t RetiringBytes() const override { return retiring; }

        // A frame: what was recorded completes when complete holds, and everything queued is recorded.
        void Frame(const std::function<bool(const TerrainPayloadKey &)> &complete = {})
        {
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Recorded && (!complete || complete(mesh.geometry)))
                    mesh.stage = Stage::Ready;
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Queued)
                    mesh.stage = Stage::Recorded;
        }

        void FailRecorded()
        {
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Recorded)
                    mesh.stage = Stage::Failed;
        }

        void FreeRetired() { retiring = 0; }

        std::map<TerrainGpuMeshId, Mesh> meshes;
        std::vector<TerrainPayloadKey> uploadOrder;
        std::vector<TerrainPayloadKey> retired;
        uint64_t retiring = 0;
        bool refuse = false;
        TerrainGpuMeshId nextId = 1;
    };

    // 4 x 4 tiles of 64 m at 4 m with an 8 m LOD, level 0 Regions of 2 x 2 tiles and one root.
    TerrainEncodedRelease ResidencyRelease(const TerrainReleaseInfo &info)
    {
        std::vector<TerrainLayout> layers;
        for (double spacing : {4.0, 8.0})
            layers.push_back(*TerrainLayout::CreateFromTileSize(64.0, spacing, {0.0, 0.0, 256.0, 256.0}));
        const std::vector<TileHeights> tiles = BuildTiles(layers[0]);
        std::vector<TerrainTileWrite> sources;
        for (const TileHeights &tile : tiles)
            sources.push_back({&tile});
        const std::vector<TerrainPayloadWrite> lods = BuildTestLods(layers, tiles);
        const TerrainRegionWrites regions = BuildTestRegions(layers[0], tiles, 2, 4);
        TerrainResult<TerrainEncodedRelease> result = EncodeTerrainRelease(info, layers, sources, lods, regions);
        INFO(result.error.message);
        REQUIRE(result);
        return std::move(*result.value);
    }

    TerrainStreamingSettings::Gpu GpuSettings(uint64_t budget, uint64_t perFrame, double retain = 1.0)
    {
        TerrainStreamingSettings::Gpu gpu;
        gpu.budgetBytes = budget;
        gpu.uploadBytesPerFrame = perFrame;
        gpu.stagingBytes = perFrame;
        gpu.meshRetainSeconds = retain;
        return gpu;
    }

    struct Release
    {
        TerrainReleaseInfo info;
        std::shared_ptr<TerrainResidentRelease> resident;
        TerrainSelectionInstance instance;

        Release(uint64_t id, glm::dvec3 at) : info(Info(TerrainAsset("terrain/residency.terrain.json"), id))
        {
            resident = TerrainResidentRelease::Create(
                OpenOk(TerrainReleaseHandle::Open(MakeTerrainMemorySource(ResidencyRelease(info)))));
            instance = {resident->SharedHandle(), {info.asset, *WorldPosition::FromDoubleMeters(at)}};
        }

        const TerrainIndex &Index() const { return resident->Handle().Index(); }

        TerrainPayloadKey Tile(TileCoord tile, uint32_t level = 0) const
        {
            return TerrainPayloadKeyOf(info, *Index().Find(tile, level, TerrainPayloadKind::LodHeight, level));
        }

        TerrainPayloadKey Region(uint32_t level, int32_t x, int32_t z) const
        {
            return TerrainPayloadKeyOf(info, *Index().FindRegion({level, x, z}));
        }

        uint64_t Bytes(const TerrainPayloadKey &key, const FakeUploader &uploader) const
        {
            const TerrainTileRecord *record =
                key.kind == TerrainPayloadKind::Region
                    ? Index().FindRegion({key.level, key.coord.x, key.coord.z})
                    : Index().Find(key.coord, key.layer, TerrainPayloadKind::LodHeight, key.level);
            return uploader.Footprint(record->vertexCount, record->indexCount);
        }
    };

    // Every payload of the releases resident on the CPU.
    struct Payloads
    {
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        TerrainStreamer streamer{StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Eager), executor};

        explicit Payloads(std::initializer_list<const Release *> releases)
        {
            for (const Release *release : releases)
                REQUIRE(streamer.Add(release->resident) == TerrainResidencyMode::Eager);
            for (int i = 0; i < 100; ++i)
            {
                streamer.Update();
                executor->RunAll();
            }
            streamer.Update();
            REQUIRE(streamer.Stats().loading + streamer.Stats().waiting == 0);
            REQUIRE(streamer.Stats().failed == 0);
        }
    };

    struct Harness
    {
        FakeUploader uploader;
        std::shared_ptr<TerrainManualExecutor> builds = std::make_shared<TerrainManualExecutor>();
        TerrainMeshResidency residency;
        double now = 0.0;

        explicit Harness(const TerrainStreamingSettings::Gpu &gpu) : residency(gpu, builds, uploader) {}

        void Request(std::span<const TerrainSelectionInstance> instances, std::vector<TerrainPayloadWant> wants,
                     const TerrainStreamer &streamer)
        {
            residency.Request(instances, wants, streamer, now);
        }

        // Builds everything queued and installs it.
        void BuildAll()
        {
            builds->RunAll();
            residency.Install();
        }
    };
}

TEST_CASE("A unit is ready only once its GPU upload has completed", "[terrain][gpu]")
{
    const Release a(0x127, {1000.5, -20.0, -2000.25});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));
    const TerrainPayloadKey tile = a.Tile({1, 2}), region = a.Region(0, 1, 0);
    const std::vector<TerrainPayloadWant> wants{{tile, Demand::Visible, 5.0f}, {region, Demand::Visible, 50.0f}};

    h.Request({&a.instance, 1}, wants, payloads.streamer);
    CHECK(h.residency.State(tile) == TerrainMeshState::Building);
    CHECK(h.residency.State(region) == TerrainMeshState::Building);
    h.residency.Install();
    CHECK(h.residency.State(tile) == TerrainMeshState::Building);
    CHECK(h.builds->Pending() == 2);

    h.BuildAll();
    CHECK(h.residency.State(tile) == TerrainMeshState::Built);
    CHECK(h.uploader.meshes.empty());
    h.Request({&a.instance, 1}, wants, payloads.streamer);
    CHECK(h.residency.State(tile) == TerrainMeshState::Uploading);
    CHECK(h.residency.State(region) == TerrainMeshState::Uploading);
    CHECK_FALSE(h.residency.Ready(tile));
    CHECK(h.residency.Acquire(tile, h.now) == nullptr);

    h.uploader.Frame();
    h.residency.Install();
    CHECK_FALSE(h.residency.Ready(tile));
    h.uploader.Frame();
    h.residency.Install();
    REQUIRE(h.residency.Ready(tile));
    REQUIRE(h.residency.Ready(region));

    const TerrainResidentMesh *mesh = h.residency.Acquire(tile, h.now);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->origin.Meters() == glm::dvec3(64.0, 0.0, 128.0));
    CHECK(h.uploader.meshes.at(mesh->id).vertices == a.Index().Find({1, 2}, 0, TerrainPayloadKind::LodHeight, 0)->vertexCount);
    const TerrainMeshStats stats = h.residency.Stats();
    CHECK(stats.ready == 2);
    CHECK(stats.heldBytes == a.Bytes(tile, h.uploader) + a.Bytes(region, h.uploader));
    CHECK(stats.buildsStarted == 2);
    CHECK(stats.uploadsReady == 2);
}

TEST_CASE("Builds start Visible before Prefetch, nearest first, a frame's upload at a time", "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    const FakeUploader sizes;
    const uint64_t tileBytes = a.Bytes(a.Tile({0, 0}), sizes);
    Harness h(GpuSettings(uint64_t(1) << 30, 2 * tileBytes));

    const std::vector<TerrainPayloadWant> wants{
        {a.Tile({3, 3}), Demand::Prefetch, 1.0f}, {a.Tile({2, 2}), Demand::Visible, 30.0f},
        {a.Tile({1, 1}), Demand::Visible, 20.0f}, {a.Tile({0, 0}), Demand::Visible, 10.0f}};
    h.Request({&a.instance, 1}, wants, payloads.streamer);
    CHECK(h.residency.State(a.Tile({0, 0})) == TerrainMeshState::Building);
    CHECK(h.residency.State(a.Tile({1, 1})) == TerrainMeshState::Building);
    CHECK(h.residency.State(a.Tile({2, 2})) == TerrainMeshState::Absent);
    CHECK(h.residency.State(a.Tile({3, 3})) == TerrainMeshState::Absent);

    REQUIRE(h.builds->RunOne());
    h.residency.Install();
    CHECK(h.residency.State(a.Tile({0, 0})) == TerrainMeshState::Built);
    CHECK(h.residency.State(a.Tile({1, 1})) == TerrainMeshState::Building);

    h.BuildAll();
    for (int frame = 0; frame < 6; ++frame)
    {
        h.Request({&a.instance, 1}, wants, payloads.streamer);
        h.BuildAll();
        h.uploader.Frame();
    }
    REQUIRE(h.uploader.uploadOrder.size() == 4);
    CHECK(h.uploader.uploadOrder[0] == a.Tile({0, 0}));
    CHECK(h.uploader.uploadOrder[1] == a.Tile({1, 1}));
    CHECK(h.uploader.uploadOrder[2] == a.Tile({2, 2}));
    CHECK(h.uploader.uploadOrder[3] == a.Tile({3, 3}));
}

TEST_CASE("Built meshes go to the uploader only while a frame's upload has room", "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    const FakeUploader sizes;
    const uint64_t tileBytes = a.Bytes(a.Tile({0, 0}), sizes);
    Harness h(GpuSettings(uint64_t(1) << 30, tileBytes));

    std::vector<TerrainPayloadWant> wants;
    for (int32_t x = 0; x < 4; ++x)
        wants.push_back({a.Tile({x, 0}), Demand::Visible, float(x)});
    for (int frame = 0; frame < 4; ++frame)
    {
        h.Request({&a.instance, 1}, wants, payloads.streamer);
        h.BuildAll();
    }
    CHECK(h.uploader.QueuedBytes() == tileBytes);
    CHECK(h.uploader.meshes.size() == 1);
    h.uploader.Frame();
    h.Request({&a.instance, 1}, wants, payloads.streamer);
    CHECK(h.uploader.meshes.size() == 2);
}

TEST_CASE("A Visible mesh makes room by retiring unwanted, then Prefetch meshes, never a Visible one",
          "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    const FakeUploader sizes;
    const uint64_t tileBytes = a.Bytes(a.Tile({0, 0}), sizes);
    Harness h(GpuSettings(3 * tileBytes, uint64_t(1) << 30, 100.0));

    const auto settle = [&](const std::vector<TerrainPayloadWant> &wants)
    {
        for (int frame = 0; frame < 4; ++frame)
        {
            h.Request({&a.instance, 1}, wants, payloads.streamer);
            h.BuildAll();
            h.uploader.Frame();
            h.uploader.Frame();
            h.residency.Install();
            h.now += 0.1;
        }
    };
    const TerrainPayloadKey drawn = a.Tile({0, 0}), far = a.Tile({3, 0}), old = a.Tile({0, 3});
    settle({{drawn, Demand::Visible, 1.0f}, {far, Demand::Prefetch, 9.0f}, {old, Demand::Visible, 5.0f}});
    REQUIRE(h.residency.Ready(drawn));
    REQUIRE(h.residency.Ready(far));
    REQUIRE(h.residency.Ready(old));

    const TerrainPayloadKey next = a.Tile({1, 1});
    h.Request({&a.instance, 1}, {{drawn, Demand::Visible, 1.0f}, {far, Demand::Prefetch, 9.0f},
                                 {next, Demand::Visible, 2.0f}}, payloads.streamer);
    CHECK(h.uploader.retired == std::vector<TerrainPayloadKey>{old});
    CHECK(h.residency.State(next) == TerrainMeshState::Absent);
    CHECK(h.residency.Stats().budgetWaits == 1);
    CHECK(h.residency.Stats().retiringBytes == tileBytes);

    h.uploader.FreeRetired();
    h.Request({&a.instance, 1}, {{drawn, Demand::Visible, 1.0f}, {far, Demand::Prefetch, 9.0f},
                                 {next, Demand::Visible, 2.0f}}, payloads.streamer);
    CHECK(h.residency.State(next) == TerrainMeshState::Building);

    const TerrainPayloadKey another = a.Tile({2, 1}), speculative = a.Tile({2, 2});
    h.Request({&a.instance, 1}, {{drawn, Demand::Visible, 1.0f}, {far, Demand::Prefetch, 9.0f},
                                 {next, Demand::Visible, 2.0f}, {another, Demand::Visible, 3.0f},
                                 {speculative, Demand::Prefetch, 1.0f}}, payloads.streamer);
    CHECK(h.uploader.retired == std::vector<TerrainPayloadKey>{old, far});
    CHECK(h.residency.Ready(drawn));
    CHECK(h.residency.State(speculative) == TerrainMeshState::Absent);
    CHECK(h.residency.Stats().evicted == 2);
}

TEST_CASE("A mesh nobody wants or draws retires after meshRetainSeconds", "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30, 2.0));
    const TerrainPayloadKey tile = a.Tile({2, 1});
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.BuildAll();
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.uploader.Frame();
    h.uploader.Frame();
    h.residency.Install();
    REQUIRE(h.residency.Acquire(tile, h.now) != nullptr);

    h.now = 1.0;
    REQUIRE(h.residency.Acquire(tile, h.now) != nullptr);
    h.now = 2.9;
    h.Request({&a.instance, 1}, {}, payloads.streamer);
    CHECK(h.residency.Ready(tile));
    h.now = 3.0;
    h.Request({&a.instance, 1}, {}, payloads.streamer);
    CHECK(h.residency.State(tile) == TerrainMeshState::Absent);
    CHECK(h.uploader.retired == std::vector<TerrainPayloadKey>{tile});
    CHECK(h.residency.Stats().retired == 1);
}

TEST_CASE("A build nobody wants any more is canceled, and a result for a dropped mesh never uploads",
          "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));
    const TerrainPayloadKey first = a.Tile({0, 1}), second = a.Tile({1, 1});

    h.Request({&a.instance, 1}, {{first, Demand::Visible, 1.0f}}, payloads.streamer);
    h.Request({&a.instance, 1}, {}, payloads.streamer);
    CHECK(h.residency.State(first) == TerrainMeshState::Absent);
    CHECK(h.residency.Stats().buildsCanceled == 1);
    h.BuildAll();
    CHECK(h.residency.State(first) == TerrainMeshState::Absent);

    h.Request({&a.instance, 1}, {{second, Demand::Visible, 1.0f}}, payloads.streamer);
    h.builds->RunAll();
    h.residency.RetireAll();
    h.residency.Install();
    h.Request({&a.instance, 1}, {}, payloads.streamer);
    CHECK(h.residency.State(second) == TerrainMeshState::Absent);
    CHECK(h.uploader.uploadOrder.empty());
}

TEST_CASE("Switching releases retires every mesh of the old one, and its late work never becomes ready",
          "[terrain][gpu]")
{
    const Release a(0xa, {0.0, 0.0, 0.0});
    const Release b(0xb, {0.0, 0.0, 0.0});
    const Payloads payloads{&a, &b};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));
    const TerrainSelectionInstance both[]{a.instance, b.instance};

    const TerrainPayloadKey ready = a.Tile({0, 0}), uploading = a.Tile({1, 0}), building = a.Tile({2, 0});
    const TerrainPayloadKey kept = b.Tile({0, 0});
    h.Request(both, {{ready, Demand::Visible, 1.0f}, {kept, Demand::Visible, 1.0f}}, payloads.streamer);
    h.BuildAll();
    h.Request(both, {{ready, Demand::Visible, 1.0f}, {kept, Demand::Visible, 1.0f}, {uploading, Demand::Visible, 2.0f}},
              payloads.streamer);
    h.uploader.Frame();
    h.uploader.Frame();
    h.BuildAll();
    REQUIRE(h.residency.Ready(ready));
    REQUIRE(h.residency.Ready(kept));
    h.Request(both, {{ready, Demand::Visible, 1.0f}, {kept, Demand::Visible, 1.0f}, {uploading, Demand::Visible, 2.0f},
                     {building, Demand::Visible, 3.0f}},
              payloads.streamer);
    REQUIRE(h.residency.State(uploading) == TerrainMeshState::Uploading);
    REQUIRE(h.residency.State(building) == TerrainMeshState::Building);
    h.builds->RunAll();

    const TerrainReleaseInfo keep[]{b.info};
    h.residency.RetireReleases(keep);
    CHECK(h.uploader.retired.size() == 2);
    h.uploader.Frame();
    h.uploader.Frame();
    h.residency.Install();
    h.Request({&b.instance, 1}, {{kept, Demand::Visible, 1.0f}}, payloads.streamer);
    for (const TerrainPayloadKey &key : {ready, uploading, building})
    {
        CHECK(h.residency.State(key) == TerrainMeshState::Absent);
        CHECK(h.residency.Acquire(key, h.now) == nullptr);
    }
    CHECK(h.residency.Ready(kept));
    CHECK(std::count(h.uploader.uploadOrder.begin(), h.uploader.uploadOrder.end(), building) == 0);
}

TEST_CASE("A failed upload is reported once and not tried again", "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));
    const TerrainPayloadKey tile = a.Tile({3, 3}), refused = a.Tile({0, 3});
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.BuildAll();
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.uploader.Frame();
    h.uploader.FailRecorded();
    h.residency.Install();
    CHECK(h.residency.State(tile) == TerrainMeshState::Failed);
    CHECK(h.uploader.retired == std::vector<TerrainPayloadKey>{tile});

    h.uploader.refuse = true;
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}, {refused, Demand::Visible, 1.0f}}, payloads.streamer);
    h.BuildAll();
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}, {refused, Demand::Visible, 1.0f}}, payloads.streamer);
    CHECK(h.residency.State(refused) == TerrainMeshState::Failed);
    const auto failures = h.residency.TakeFailures();
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].first == tile);
    CHECK(failures[1].first == refused);
    CHECK(h.residency.TakeFailures().empty());
    CHECK(h.residency.Stats().buildsStarted == 2);
    CHECK(h.residency.Stats().heldBytes == 0);
}

TEST_CASE("Forgetting drops every mesh without retiring any", "[terrain][gpu]")
{
    const Release a(0x127, {0.0, 0.0, 0.0});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));
    const TerrainPayloadKey tile = a.Tile({1, 3});
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.BuildAll();
    h.Request({&a.instance, 1}, {{tile, Demand::Visible, 1.0f}}, payloads.streamer);
    h.uploader.Frame();
    h.uploader.Frame();
    h.residency.Install();
    REQUIRE(h.residency.Ready(tile));
    h.residency.Forget();
    CHECK(h.residency.State(tile) == TerrainMeshState::Absent);
    CHECK(h.uploader.retired.empty());
    CHECK(h.residency.Stats().heldBytes == 0);
}

TEST_CASE("A refined Region stays drawn until every child's GPU mesh is ready", "[terrain][gpu][selection]")
{
    const Release a(0x127, {1000.5, -20.0, -2000.25});
    const Payloads payloads{&a};
    Harness h(GpuSettings(uint64_t(1) << 30, uint64_t(1) << 30));

    TerrainStreamingSettings::View view;
    view.refineErrorPixels = 1.0;
    view.mergeErrorPixels = 0.75;
    view.prefetchErrorPixels = 0.5;
    view.prefetchBudgetBytes = 0;
    TerrainViewSelector selector(view);
    const glm::dvec3 eye = glm::dvec3(1000.5, -20.0, -2000.25) + glm::dvec3(40.0, 30.0, 40.0);
    const glm::mat4 viewProj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.5f, 4000.0f) *
                               glm::lookAt(glm::vec3(0.0f), glm::vec3(0.3f, -1.0f, 0.2f), glm::vec3(0.0f, 1.0f, 0.0f));
    const TerrainView camera{{*WorldPosition::FromDoubleMeters(eye), viewProj}, 1000.0, 0.0};
    const TerrainUnitReady ready = [&](const TerrainDrawUnit &unit) { return h.residency.Ready(unit.geometry); };

    const auto frame = [&](const std::function<bool(const TerrainPayloadKey &)> &complete)
    {
        h.residency.Install();
        selector.Update({&a.instance, 1}, camera, ready);
        h.Request({&a.instance, 1}, std::vector<TerrainPayloadWant>(selector.Wants().begin(), selector.Wants().end()),
                  payloads.streamer);
        for (const TerrainDrawUnit &unit : selector.Draws())
            REQUIRE(h.residency.Acquire(unit.geometry, h.now) != nullptr);
        h.BuildAll();
        h.uploader.Frame(complete);
        h.now += 1.0 / 60.0;
    };

    const auto regionsOnly = [](const TerrainPayloadKey &key) { return key.kind == TerrainPayloadKind::Region; };
    bool regionDrawn = false;
    for (int i = 0; i < 30; ++i)
    {
        frame(regionsOnly);
        for (const TerrainDrawUnit &unit : selector.Draws())
            CHECK(unit.kind == TerrainUnitKind::Region);
        regionDrawn = regionDrawn || !selector.Draws().empty();
    }
    CHECK(regionDrawn);
    CHECK(selector.Stats().missing == 0);
    CHECK(selector.Stats().coarserFallbacks > 0);

    for (int i = 0; i < 30; ++i)
        frame({});
    CHECK(selector.Stats().coarserFallbacks == 0);
    CHECK(selector.Stats().missing == 0);
    CHECK(std::any_of(selector.Draws().begin(), selector.Draws().end(),
                      [](const TerrainDrawUnit &unit) { return unit.kind == TerrainUnitKind::Tile; }));
}
