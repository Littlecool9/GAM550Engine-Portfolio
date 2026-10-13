/**
 * @file TerrainReleaseSwitch.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainReleaseSwitch.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainResidencyTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainReleaseSwitch.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainSurface.h>
#include <terrain/TerrainViewSelection.h>
#include <terrain/components/TerrainInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Kind = TerrainSwitchEvent::Kind;
    using State = TerrainSwitchState;

    constexpr std::string_view kIdRoot = "games/TerrainFixture/assets";
    constexpr std::string_view kRecipe = "terrain/switch.terrain.json";
    constexpr double kStep = 1.0 / 60.0;
    constexpr float kSaltB = 7.5f;
    // Below every height of the fixture, so a source there always needs its pieces.
    constexpr double kBodyY = -100.0;
    const glm::dvec3 kPlacement{1000.5, -20.0, -2000.25};

    TerrainLayout SourceLayout()
    {
        return *TerrainLayout::CreateFromTileSize(64.0, 4.0, {0.0, 0.0, 256.0, 256.0});
    }

    // 4 x 4 tiles of 64 m at 4 m with an 8 m LOD, level 0 Regions of 2 x 2 tiles and one root.
    TerrainEncodedRelease SwitchRelease(const TerrainReleaseInfo &info, float salt)
    {
        std::vector<TerrainLayout> layers{SourceLayout(),
                                          *TerrainLayout::CreateFromTileSize(64.0, 8.0, {0.0, 0.0, 256.0, 256.0})};
        const std::vector<TileHeights> tiles = BuildTiles(layers[0], salt);
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

    TerrainReleaseInfo Publish(const std::filesystem::path &root, AssetId asset, uint64_t release, float salt)
    {
        const TerrainReleaseInfo info = Info(asset, release);
        WriteRelease(root, info, SwitchRelease(info, salt));
        PointCurrentAt(root, asset, info.release);
        return info;
    }

    TerrainStreamingSettings NoPrefetch(TerrainStreamingSettings settings)
    {
        settings.view.prefetchBudgetBytes = 0;
        return settings;
    }

    std::shared_ptr<TerrainResidentRelease> InMemory(const TerrainReleaseInfo &info, float salt)
    {
        return TerrainResidentRelease::Create(
            OpenOk(TerrainReleaseHandle::Open(MakeTerrainMemorySource(SwitchRelease(info, salt)))));
    }

    // Range reads wait while held, each until it is let go; whole-file reads pass.
    class GatedSource final : public TerrainByteSource
    {
    public:
        explicit GatedSource(std::shared_ptr<TerrainByteSource> inner) : mInner(std::move(inner)) {}

        // Up to limit reads wait at once; later ones pass.
        void Hold(size_t limit)
        {
            std::lock_guard lock(mMutex);
            mHeld = true;
            mLimit = limit;
        }

        void Open()
        {
            {
                std::lock_guard lock(mMutex);
                mHeld = false;
            }
            mChanged.notify_all();
        }

        size_t Waiting() const
        {
            std::lock_guard lock(mMutex);
            return mWaiting.size();
        }

        // The waiting read that started at, oldest first.
        void Let(size_t at)
        {
            {
                std::lock_guard lock(mMutex);
                REQUIRE(at < mWaiting.size());
                mLet.insert(mWaiting[at]);
            }
            mChanged.notify_all();
        }

        std::string Describe() const override { return mInner->Describe(); }

    protected:
        std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const override
        {
            return mInner->ReadAll(file);
        }

        std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                          size_t length) const override
        {
            {
                std::unique_lock lock(mMutex);
                if (mHeld && mWaiting.size() < mLimit)
                {
                    const uint64_t ticket = mNext++;
                    mWaiting.push_back(ticket);
                    mChanged.wait(lock, [&] { return !mHeld || mLet.contains(ticket); });
                    std::erase(mWaiting, ticket);
                    mLet.erase(ticket);
                }
            }
            return mInner->ReadRange(file, offset, length);
        }

        std::optional<uint64_t> DoSize(std::string_view file) const override { return mInner->Size(file); }

    private:
        std::shared_ptr<TerrainByteSource> mInner;
        mutable std::mutex mMutex;
        mutable std::condition_variable mChanged;
        bool mHeld = false;
        size_t mLimit = 0;
        mutable uint64_t mNext = 0;
        mutable std::vector<uint64_t> mWaiting;
        mutable std::set<uint64_t> mLet;
    };

    // Removed after every worker has stopped reading it.
    struct ScratchRoot
    {
        std::filesystem::path path = MakeScratchDir("switch");

        ~ScratchRoot()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    // The app's terrain composition without GPU or Jolt: one view, one collision source, Play and Stop.
    struct Runtime
    {
        ScratchRoot scratch;
        std::filesystem::path root = scratch.path;
        AssetId asset = TerrainAsset(kRecipe);
        TerrainStreamingSettings settings = NoPrefetch(StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Interest));
        std::shared_ptr<TerrainWorkerPool> pool = std::make_shared<TerrainWorkerPool>(4);
        std::shared_ptr<TerrainManualExecutor> checks = std::make_shared<TerrainManualExecutor>();
        std::map<uint64_t, std::shared_ptr<GatedSource>> sources;
        int opens = 0;
        TerrainReleaseOpener open = [this](AssetId id, TerrainReleaseId release) -> TerrainResult<TerrainReleaseHandle>
        {
            ++opens;
            auto source = std::make_shared<GatedSource>(
                std::make_shared<TerrainFileSource>(TerrainReleaseDir(root, id, release)));
            sources[release.value] = source;
            return TerrainReleaseHandle::Open(source);
        };
        TerrainStreamer streamer{settings, pool};
        FakeUploader uploader;
        TerrainMeshResidency meshes{settings.gpu, pool, uploader};
        std::shared_ptr<FakePhysics> physics = std::make_shared<FakePhysics>();
        TerrainCollisionResidency collision{settings.collision, pool, physics};
        TerrainReleaseSwitch releases{checks, root, open};
        World world;
        TerrainSceneSpec spec;
        TerrainWorld terrain;
        TerrainViewSelector view{settings.view};
        TerrainInterestId interest = streamer.AddInterest();
        TerrainInterestId probe = streamer.AddInterest();
        std::map<uint64_t, std::unique_ptr<TerrainViewSelector>> stagedViews;
        std::map<uint64_t, bool> stagedWarm;
        std::vector<TerrainDrawUnit> draws;
        std::vector<TerrainSwitchEvent> events;
        std::vector<TerrainReleaseAdoption> adopted;
        uint64_t stagingVersion = 0;
        glm::dvec3 eye{128.0, 140.0, 128.0};
        glm::dvec3 body{32.0, kBodyY, 32.0};
        bool playing = false;
        double now = 0.0;

        Runtime()
        {
            InstallHierarchy(world);
            RegisterCoreComponents();
            RegisterTerrainComponents();
        }

        // Held reads would keep a worker from joining.
        ~Runtime()
        {
            for (auto &[id, source] : sources)
                source->Open();
        }

        WorldPosition At(const glm::dvec3 &local) const { return *WorldPosition::FromDoubleMeters(kPlacement + local); }

        void Load()
        {
            const Entity entity = world.CreateEntity();
            AddTransform(world, entity, Transform(*TryWorldVec3FromDouble(kPlacement)));
            TerrainInstance instance{};
            std::memcpy(instance.recipePath, kRecipe.data(), kRecipe.size());
            world.AddComponent(entity, instance);
            spec = CollectTerrainScene(world, kIdRoot);
            REQUIRE(spec.instances.size() == 1);
            Apply({});
        }

        TerrainReleaseInfo Loaded() const
        {
            REQUIRE(terrain.Instances().size() == 1);
            return terrain.Instances()[0].release->Info();
        }

        void Apply(std::vector<TerrainReleaseAdoption> adoptions)
        {
            TerrainResolveOptions options;
            options.keepLoaded = true;
            options.open = open;
            for (const TerrainReleaseAdoption &adoption : adoptions)
                options.adopt.push_back(adoption.release);
            std::vector<TerrainIssue> issues;
            TerrainWorld next = ResolveTerrainWorld(spec, root, &terrain, issues, streamer, options);
            terrain = std::move(next);
            adopted.insert(adopted.end(), adoptions.begin(), adoptions.end());
            Residencies();
        }

        // What the app hands its residencies whenever the loaded or staged releases change.
        void Residencies()
        {
            std::vector<TerrainReleaseInfo> keep;
            std::vector<TerrainCollisionInstance> active, staged;
            for (const TerrainInstanceState &instance : terrain.Instances())
            {
                keep.push_back(instance.release->Info());
                active.push_back({instance.release->SharedHandle(), instance.placement,
                                  *streamer.ModeOf(instance.release->Info())});
                for (const std::shared_ptr<TerrainResidentRelease> &release : releases.Staged())
                    if (release->Info().asset == instance.placement.asset)
                    {
                        keep.push_back(release->Info());
                        staged.push_back({release->SharedHandle(), instance.placement, *streamer.ModeOf(release->Info())});
                    }
            }
            meshes.RetireReleases(keep);
            collision.SetInstances(active, staged);
            stagingVersion = releases.StagingVersion();
        }

        TerrainCollisionSource Source() const
        {
            TerrainCollisionSource source;
            source.position = At(body);
            source.radius = 0.5;
            source.acceleration = 9.81;
            source.tolerance = settings.collision.bodyToleranceMeters;
            return source;
        }

        void Play()
        {
            playing = true;
            releases.Suspend();
            Residencies();
        }

        void Stop() { playing = false; }

        // One frame: the install point, the Edit safe point, then the view.
        void Frame()
        {
            streamer.Update();
            releases.Update(streamer);
            if (releases.StagingVersion() != stagingVersion)
                Residencies();
            meshes.Install();
            collision.Install(now);
            const std::vector<TerrainCollisionSource> bodies{Source()};
            collision.Update(bodies, streamer, now, playing ? kStep : 0.0);
            for (const std::shared_ptr<TerrainResidentRelease> &release : releases.Staged())
                releases.Report(release->Info().asset,
                                MeasureTerrainPrewarm(release->Info(), streamer, &meshes, &collision,
                                                      stagedWarm[release->Info().asset.value]));

            if (!playing)
            {
                releases.Resume();
                if (releases.Poll(terrain))
                    Apply({});
                if (std::vector<TerrainReleaseAdoption> ready = releases.TakeReady(); !ready.empty())
                    Apply(std::move(ready));
            }
            const std::vector<TerrainSwitchEvent> taken = releases.TakeEvents();
            events.insert(events.end(), taken.begin(), taken.end());

            Prepare();
            checks->RunAll();
            uploader.Frame();
            now += kStep;
        }

        void Prepare()
        {
            const glm::mat4 viewProj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.5f, 4000.0f) *
                                       glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.001f),
                                                   glm::vec3(0.0f, 1.0f, 0.0f));
            const TerrainView camera{{At(eye), viewProj}, 1000.0, 0.0};
            const TerrainUnitReady ready = [this](const TerrainDrawUnit &unit) { return meshes.Ready(unit.geometry); };

            std::vector<TerrainSelectionInstance> instances;
            for (const TerrainInstanceState &instance : terrain.Instances())
                instances.push_back({instance.release->SharedHandle(), instance.placement});
            view.Update(instances, camera, ready);
            std::vector<TerrainPayloadWant> wants(view.Wants().begin(), view.Wants().end());

            stagedWarm.clear();
            std::set<uint64_t> staged;
            for (const std::shared_ptr<TerrainResidentRelease> &release : releases.Staged())
            {
                std::vector<TerrainSelectionInstance> stagedInstances;
                for (const TerrainInstanceState &instance : terrain.Instances())
                    if (instance.placement.asset == release->Info().asset)
                        stagedInstances.push_back({release->SharedHandle(), instance.placement});
                std::unique_ptr<TerrainViewSelector> &selector = stagedViews[release->Info().asset.value];
                if (!selector)
                    selector = std::make_unique<TerrainViewSelector>(settings.view);
                selector->Update(stagedInstances, camera, ready);
                const TerrainViewStats &stats = selector->Stats();
                stagedWarm[release->Info().asset.value] =
                    stats.missing == 0 && stats.coarserFallbacks == 0 && stats.otherFallbacks == 0;
                for (const TerrainPayloadWant &want : selector->Wants())
                    if (want.demand == TerrainDemand::Visible)
                        wants.push_back({want.key, TerrainDemand::Prefetch, want.rank});
                instances.insert(instances.end(), stagedInstances.begin(), stagedInstances.end());
                staged.insert(release->Info().asset.value);
            }
            std::erase_if(stagedViews, [&](const auto &entry) { return !staged.contains(entry.first); });

            streamer.SetInterest(interest, wants);
            meshes.Request(instances, wants, streamer, now);
            draws.clear();
            for (const TerrainDrawUnit &unit : view.Draws())
                if (meshes.Acquire(unit.geometry, now))
                    draws.push_back(unit);
        }

        template <typename Done>
        void FrameUntil(Done done, std::source_location from = std::source_location::current())
        {
            INFO("waiting for line " << from.line());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!done())
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                Frame();
                streamer.WaitForCompletion(std::chrono::milliseconds(1));
            }
        }

        bool Settled()
        {
            const TerrainViewStats &stats = view.Stats();
            return !draws.empty() && stats.missing == 0 && stats.coarserFallbacks == 0 && stats.otherFallbacks == 0 &&
                   !physics->placed.empty() && collision.Answer(0).status.state == TerrainCollisionState::Ready &&
                   terrain.HeightAt(At(body), 4.0).HasHeight();
        }

        size_t Count(Kind kind) const
        {
            return size_t(std::count_if(events.begin(), events.end(),
                                        [&](const TerrainSwitchEvent &event) { return event.kind == kind; }));
        }

        // Whatever is drawn, collided with or answered comes from release; loading is allowed.
        void OnlyFrom(const TerrainReleaseInfo &release, float salt)
        {
            for (const TerrainDrawUnit &unit : draws)
            {
                CHECK(unit.geometry.release == release.release);
                const TerrainResidentMesh *mesh = meshes.Acquire(unit.geometry, now);
                REQUIRE(mesh != nullptr);
                CHECK(uploader.meshes.at(mesh->id).geometry.release == release.release);
            }
            for (const auto &[id, placed] : physics->placed)
                CHECK(placed.shape->release == release);
            for (const TerrainColliderId id : collision.Answer(0).contacts)
                CHECK(physics->placed.contains(id));

            const TerrainLayout layout = SourceLayout();
            const std::vector<TileHeights> tiles = BuildTiles(layout, salt);
            for (const glm::dvec3 &local : {body, glm::dvec3{200.5, 0.0, 17.25}, glm::dvec3{64.0, 0.0, 192.0}})
            {
                const TerrainHeightResult result = terrain.HeightAt(At(local), 4.0);
                if (result.status == TerrainHeightStatus::Loading)
                    continue;
                REQUIRE(result.status == TerrainHeightStatus::MeetsRequirement);
                CHECK(result.release == release);
                const SurfaceSample expected = EvaluateSurface(
                    layout, *TerrainLocalPosition::FromMeters({local.x, 0.0, local.z}),
                    [&](TileCoord tile)
                    {
                        for (const TileHeights &t : tiles)
                            if (t.Tile() == tile)
                                return t.Samples();
                        return std::span<const float>();
                    });
                REQUIRE(expected.IsOk());
                CHECK(result.localHeight == Catch::Approx(expected.height).margin(1e-9));
            }
        }

        void Observe(const TerrainReleaseInfo &release, float salt)
        {
            REQUIRE(Settled());
            OnlyFrom(release, salt);
        }

        // The loaded release's range reads now wait; a probe asks for payloads it has not read yet.
        GatedSource &HoldNewReads(size_t limit, std::source_location from = std::source_location::current())
        {
            const TerrainReleaseInfo loaded = Loaded();
            GatedSource &reads = *sources.at(loaded.release.value);
            reads.Hold(limit);
            std::vector<TerrainPayloadWant> wants;
            const TerrainIndex &index = terrain.Instances()[0].release->Handle().Index();
            for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
                for (const TerrainTileRecord &record : *records)
                {
                    const TerrainPayloadKey key = TerrainPayloadKeyOf(loaded, record);
                    if (wants.size() < limit && streamer.Status(key).state == TerrainPayloadState::Absent)
                        wants.push_back({key, TerrainDemand::Visible, 0.0f});
                }
            REQUIRE(wants.size() == limit);
            streamer.SetInterest(probe, wants);
            FrameUntil([&] { return reads.Waiting() >= limit; }, from);
            return reads;
        }
    };

    std::vector<std::filesystem::path> FilesOf(const std::filesystem::path &dir)
    {
        std::vector<std::filesystem::path> files;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir))
            files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        return files;
    }

    void FlipLastByte(const std::filesystem::path &file)
    {
        std::optional<std::vector<std::byte>> bytes = ReadBytes(file);
        REQUIRE(bytes);
        bytes->back() ^= std::byte{0x40};
        REQUIRE(WriteBytes(file, *bytes));
    }

    bool Untouched(const TerrainReleaseUsage &usage)
    {
        return usage.waiting + usage.inFlight + usage.ready + usage.failed == 0 && usage.bytes == 0;
    }
}

TEST_CASE("Play pins release A while B is published, then the Edit switch lets no late A read into B's scene",
          "[terrain][switch]")
{
    Runtime r;
    const TerrainReleaseInfo a = Publish(r.root, r.asset, 0xA, 0.0f);
    r.Load();
    REQUIRE(r.Loaded() == a);
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(a, 0.0f);

    r.Play();
    const TerrainReleaseInfo b = Publish(r.root, r.asset, 0xB, kSaltB);
    const std::vector<std::filesystem::path> filesA = FilesOf(TerrainReleaseDir(r.root, r.asset, a.release));

    r.eye = {224.0, 40.0, 224.0};
    r.body = {224.0, kBodyY, 224.0};
    GatedSource &readsA = r.HoldNewReads(3);
    while (readsA.Waiting() > 0)
    {
        const uint64_t installed = r.streamer.Stats().readsSucceeded;
        readsA.Let(readsA.Waiting() - 1);
        r.FrameUntil([&] { return r.streamer.Stats().readsSucceeded > installed; });
        r.OnlyFrom(a, 0.0f);
    }
    readsA.Open();
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(a, 0.0f);
    CHECK(r.Loaded() == a);
    CHECK(r.releases.Candidates().empty());
    CHECK(Untouched(r.streamer.Usage(b)));
    CHECK(r.opens == 1);
    CHECK(FilesOf(TerrainReleaseDir(r.root, r.asset, a.release)) == filesA);

    r.eye = {40.0, 40.0, 200.0};
    r.body = {40.0, kBodyY, 200.0};
    r.HoldNewReads(2);
    const uint64_t discarded = r.streamer.Stats().discarded;
    r.Stop();
    r.FrameUntil(
        [&]
        {
            if (r.Loaded() == a)
                r.OnlyFrom(a, 0.0f);
            return r.Loaded() == b;
        });
    CHECK(r.Count(Kind::Started) == 1);
    CHECK(r.Count(Kind::Verified) == 1);
    CHECK(r.Count(Kind::Ready) == 1);
    REQUIRE(r.adopted.size() == 1);
    const TerrainReleaseCandidate &moved = r.adopted[0].candidate;
    CHECK(moved.loaded == a.release);
    CHECK(moved.release == b.release);
    CHECK(moved.packs > 0);
    CHECK(moved.packsVerified == moved.packs);
    CHECK(moved.bytesVerified > 0);
    REQUIRE(moved.stored.size() == 2);
    CHECK(moved.stored[0].release == a.release);
    CHECK(moved.stored[0].files == filesA.size());
    CHECK(moved.stored[1].release == b.release);

    REQUIRE(readsA.Waiting() == 2);
    readsA.Let(1);
    readsA.Let(0);
    r.FrameUntil([&] { return r.streamer.Stats().discarded >= discarded + 2 && r.Settled(); });
    r.Observe(b, kSaltB);
    CHECK(Untouched(r.streamer.Usage(a)));
    CHECK(Untouched(r.meshes.Usage(a)));
    CHECK(Untouched(r.collision.Usage(a)));
    CHECK(FilesOf(TerrainReleaseDir(r.root, r.asset, a.release)) == filesA);

    r.Play();
    r.eye = {128.0, 140.0, 128.0};
    r.body = {150.0, kBodyY, 90.0};
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(b, kSaltB);
    CHECK(r.Loaded() == b);
}

TEST_CASE("A corrupt, truncated or partial B keeps A in use, is reported once and is not read again",
          "[terrain][switch]")
{
    Runtime r;
    const TerrainReleaseInfo a = Publish(r.root, r.asset, 0xA, 0.0f);
    r.Load();
    r.FrameUntil([&] { return r.Settled(); });

    const TerrainReleaseInfo b = Publish(r.root, r.asset, 0xB, kSaltB);
    const std::filesystem::path dirB = TerrainReleaseDir(r.root, r.asset, b.release);
    std::string expected;
    SECTION("A pack with a flipped byte")
    {
        FlipLastByte(dirB / "chunk_0_0.tpack");
        expected = "checksum";
    }
    SECTION("A truncated pack")
    {
        std::optional<std::vector<std::byte>> bytes = ReadBytes(dirB / "chunk_0_0.tpack");
        REQUIRE(bytes);
        bytes->resize(bytes->size() / 2);
        REQUIRE(WriteBytes(dirB / "chunk_0_0.tpack", *bytes));
        expected = "truncated";
    }
    SECTION("A pack left out of the publish")
    {
        std::filesystem::remove(dirB / "chunk_0_0.tpack");
        expected = "missing pack";
    }
    SECTION("No index")
    {
        std::filesystem::remove(dirB / kTerrainIndexFileName);
        expected = "index";
    }

    r.FrameUntil([&] { return r.Count(Kind::Failed) == 1; });
    const auto failed = std::find_if(r.events.begin(), r.events.end(),
                                     [](const TerrainSwitchEvent &event) { return event.kind == Kind::Failed; });
    CHECK(failed->candidate.loaded == a.release);
    CHECK(failed->candidate.release == b.release);
    UNSCOPED_INFO(failed->candidate.failure);
    CHECK(failed->candidate.failure.find(expected) != std::string::npos);

    const int opens = r.opens;
    REQUIRE(r.sources.contains(b.release.value));
    const uint64_t readsB = r.sources.at(b.release.value)->Stats().reads;
    for (int i = 0; i < 30; ++i)
        r.Frame();
    CHECK(r.Count(Kind::Failed) == 1);
    CHECK(r.opens == opens);
    CHECK(r.sources.at(b.release.value)->Stats().reads == readsB);
    CHECK(r.releases.Staged().empty());
    CHECK(Untouched(r.streamer.Usage(b)));
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(a, 0.0f);

    const TerrainReleaseInfo c = Publish(r.root, r.asset, 0xC, 2.0f);
    r.FrameUntil([&] { return r.Loaded() == c && r.Settled(); });
    r.Observe(c, 2.0f);
    CHECK(r.Count(Kind::Failed) == 1);
}

TEST_CASE("Play entered before B is verified runs on A without waiting, and B's late reads leave it alone",
          "[terrain][switch]")
{
    Runtime r;
    const TerrainReleaseInfo a = Publish(r.root, r.asset, 0xA, 0.0f);
    r.Load();
    r.FrameUntil([&] { return r.Settled(); });

    const TerrainReleaseInfo b = Publish(r.root, r.asset, 0xB, kSaltB);
    r.releases.Poll(r.terrain);
    REQUIRE(r.checks->RunOne());
    r.Play();
    for (int i = 0; i < 10; ++i)
        r.Frame();
    REQUIRE(r.releases.Candidates().size() == 1);
    CHECK(r.releases.Candidates()[0].state == State::Verifying);
    CHECK(r.releases.Candidates()[0].packsVerified == 0);
    CHECK(r.releases.Staged().empty());
    CHECK(r.sources.at(b.release.value)->Stats().reads == 1);
    CHECK(Untouched(r.streamer.Usage(b)));
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(a, 0.0f);

    GatedSource &readsB = *r.sources.at(b.release.value);
    readsB.Hold(2);
    r.Stop();
    r.FrameUntil([&] { return readsB.Waiting() == 2; });
    CHECK(r.Loaded() == a);
    REQUIRE(r.releases.Staged().size() == 1);

    r.Play();
    CHECK(r.releases.Staged().empty());
    const uint64_t discarded = r.streamer.Stats().discarded;
    readsB.Let(1);
    readsB.Let(0);
    r.FrameUntil([&] { return r.streamer.Stats().discarded >= discarded + 2; });
    r.FrameUntil([&] { return Untouched(r.streamer.Usage(b)) && Untouched(r.meshes.Usage(b)); });
    CHECK(Untouched(r.collision.Usage(b)));
    r.FrameUntil([&] { return r.Settled(); });
    r.Observe(a, 0.0f);
    CHECK(r.Loaded() == a);

    readsB.Open();
    r.Stop();
    r.FrameUntil([&] { return r.Loaded() == b && r.Settled(); });
    r.Observe(b, kSaltB);
    CHECK(r.Count(Kind::Verified) == 1);
}

TEST_CASE("A candidate goes when current moves on, names the loaded release again, or the asset leaves",
          "[terrain][switch]")
{
    Runtime r;
    const TerrainReleaseInfo a = Publish(r.root, r.asset, 0xA, 0.0f);
    r.Load();

    const TerrainReleaseInfo b = Publish(r.root, r.asset, 0xB, kSaltB);
    CHECK_FALSE(r.releases.Poll(r.terrain));
    REQUIRE(r.releases.Candidates().size() == 1);
    CHECK(r.releases.Candidates()[0].release == b.release);
    CHECK(r.releases.Candidates()[0].state == State::Verifying);
    CHECK_FALSE(r.releases.Poll(r.terrain));
    CHECK(r.checks->Pending() == 1);

    const TerrainReleaseInfo c = Publish(r.root, r.asset, 0xC, 1.0f);
    r.releases.Poll(r.terrain);
    REQUIRE(r.releases.Candidates().size() == 1);
    CHECK(r.releases.Candidates()[0].release == c.release);
    r.checks->RunAll();
    r.releases.Update(r.streamer);
    CHECK(r.releases.Stats().discarded == 1);
    std::vector<TerrainSwitchEvent> events = r.releases.TakeEvents();
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == Kind::Started);
    CHECK(events[1].kind == Kind::Dropped);
    CHECK(events[1].candidate.release == b.release);
    CHECK(events[2].kind == Kind::Started);

    PointCurrentAt(r.root, r.asset, a.release);
    r.releases.Poll(r.terrain);
    CHECK(r.releases.Candidates().empty());
    events = r.releases.TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Dropped);
    CHECK(events[0].candidate.release == c.release);

    PointCurrentAt(r.root, r.asset, b.release);
    r.releases.Poll(r.terrain);
    REQUIRE(r.releases.Candidates().size() == 1);
    r.releases.Poll(TerrainWorld{});
    CHECK(r.releases.Candidates().empty());

    REQUIRE(WriteText(TerrainCurrentPointerPath(r.root, r.asset), "not a release"));
    r.releases.TakeEvents();
    r.releases.Poll(r.terrain);
    r.releases.Poll(r.terrain);
    events = r.releases.TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Unreadable);
    CHECK(events[0].candidate.loaded == a.release);
    CHECK(r.releases.Candidates().empty());
    CHECK(r.Loaded() == a);
}

TEST_CASE("An asset with no loaded release is resolved again when its current moves", "[terrain][switch]")
{
    Runtime r;
    r.Load();
    REQUIRE(r.terrain.Assets().size() == 1);
    CHECK_FALSE(r.terrain.Assets()[0].release);
    CHECK_FALSE(r.releases.Poll(r.terrain));

    const TerrainReleaseInfo a = Publish(r.root, r.asset, 0xA, 0.0f);
    CHECK(r.releases.Poll(r.terrain));
    CHECK(r.releases.Candidates().empty());
    r.Apply({});
    CHECK(r.Loaded() == a);
    CHECK_FALSE(r.releases.Poll(r.terrain));
}

TEST_CASE("A staged release's mode ignores its asset's loaded release, and its Eager payloads only use room left",
          "[terrain][switch][streaming]")
{
    const AssetId asset = TerrainAsset(kRecipe);
    const TerrainReleaseInfo infoA = Info(asset, 0xA), infoB = Info(asset, 0xB);
    const std::shared_ptr<TerrainResidentRelease> a = InMemory(infoA, 0.0f), b = InMemory(infoB, kSaltB);
    uint64_t footprint = 0;
    for (const std::vector<TerrainTileRecord> *records : {&a->Handle().Index().tiles, &a->Handle().Index().regions})
        for (const TerrainTileRecord &record : *records)
            footprint += TerrainPayloadFootprint(record);

    const auto executor = std::make_shared<TerrainManualExecutor>();
    TerrainStreamer streamer(StreamingSettings(footprint + footprint / 2), executor);
    REQUIRE(streamer.Add(a) == TerrainResidencyMode::Eager);
    REQUIRE(streamer.Add(b, true) == TerrainResidencyMode::Eager);
    for (int i = 0; i < 200; ++i)
    {
        streamer.Update();
        executor->RunAll();
        CHECK(streamer.Stats().overBudgetBytes == 0);
    }
    const TerrainReleaseUsage usageA = streamer.Usage(infoA), usageB = streamer.Usage(infoB);
    CHECK(usageA.bytes == footprint);
    CHECK(usageA.waiting + usageA.inFlight == 0);
    CHECK(usageB.ready > 0);
    CHECK(usageB.waiting > 0);
    CHECK(usageB.bytes <= footprint / 2);

    streamer.Add(b);
    for (int i = 0; i < 200; ++i)
    {
        streamer.Update();
        executor->RunAll();
    }
    CHECK(streamer.Usage(infoB).bytes == footprint);
    CHECK(streamer.Usage(infoB).waiting == 0);
    CHECK(streamer.Stats().overBudgetBytes > 0);
}

TEST_CASE("Staged collision pieces are built ahead but placed only once their release is set active",
          "[terrain][switch][collision]")
{
    const AssetId asset = TerrainAsset(kRecipe);
    const TerrainReleaseInfo infoA = Info(asset, 0xA), infoB = Info(asset, 0xB);
    for (const TerrainResidencyMode mode : {TerrainResidencyMode::Eager, TerrainResidencyMode::Interest})
    {
        INFO("mode " << TerrainResidencyModeName(mode));
        const std::shared_ptr<TerrainResidentRelease> a = InMemory(infoA, 0.0f), b = InMemory(infoB, kSaltB);
        const auto executor = std::make_shared<TerrainManualExecutor>();
        const TerrainStreamingSettings settings = StreamingSettings(uint64_t(1) << 30, mode);
        TerrainStreamer streamer(settings, executor);
        streamer.Add(a);
        streamer.Add(b, true);
        const auto physics = std::make_shared<FakePhysics>();
        TerrainCollisionResidency collision(settings.collision, executor, physics);
        const TerrainPlacement placement{asset, *WorldPosition::FromDoubleMeters(kPlacement)};
        const std::vector<TerrainCollisionInstance> active{{a->SharedHandle(), placement, mode}};
        const std::vector<TerrainCollisionInstance> staged{{b->SharedHandle(), placement, mode}};
        collision.SetInstances(active, staged);

        TerrainCollisionSource source;
        source.position = *WorldPosition::FromDoubleMeters(kPlacement + glm::dvec3(32.0, kBodyY, 32.0));
        source.radius = 0.5;
        source.acceleration = 9.81;
        source.tolerance = settings.collision.bodyToleranceMeters;
        const std::vector<TerrainCollisionSource> sources{source};
        double now = 0.0;
        for (int i = 0; i < 20; ++i)
        {
            streamer.Update();
            collision.Install(now);
            collision.Update(sources, streamer, now, kStep);
            executor->RunAll();
            now += kStep;
        }

        REQUIRE(collision.Answer(0).status.state == TerrainCollisionState::Ready);
        const TerrainCollisionStatus prewarmed = collision.StagedStatus(infoB);
        CHECK(prewarmed.state == TerrainCollisionState::Ready);
        CHECK(prewarmed.needed == collision.Answer(0).status.needed);
        CHECK(prewarmed.ready == prewarmed.needed);
        CHECK(collision.Usage(infoB).ready > 0);
        REQUIRE_FALSE(physics->placed.empty());
        for (const auto &[id, placed] : physics->placed)
            CHECK(placed.shape->release == infoA);
        bool builtAhead = false;
        for (const TerrainCollisionEvent &event : collision.TakeEvents())
            if (event.piece.release == infoB.release)
            {
                CHECK(event.staged);
                CHECK(event.colliders == 0);
                builtAhead = true;
            }
        CHECK(builtAhead);

        const int builds = physics->builds;
        collision.SetInstances(staged);
        CHECK(physics->placed.empty());
        collision.Install(now);
        collision.Update(sources, streamer, now, kStep);
        CHECK(physics->builds == builds);
        REQUIRE(collision.Answer(0).status.state == TerrainCollisionState::Ready);
        REQUIRE_FALSE(physics->placed.empty());
        for (const auto &[id, placed] : physics->placed)
            CHECK(placed.shape->release == infoB);
        CHECK(Untouched(collision.Usage(infoA)));
    }
}
