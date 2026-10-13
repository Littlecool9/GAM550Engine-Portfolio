/**
 * @file TerrainCollisionResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainCollisionResidency.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using State = TerrainCollisionState;
    constexpr double kStep = 1.0 / 60.0;

    class FakeShape final : public TerrainColliderShape
    {
    public:
        double spacing = 0.0;
        uint32_t countX = 0;
        uint32_t countZ = 0;
        GlobalSampleIndex first;
        TerrainLocalPosition origin;
        float quantizationMin = 0.0f;
        float quantizationMax = 0.0f;
    };

    // Shapes are counted, colliders recorded by id.
    class FakePhysics final : public TerrainColliderPhysics
    {
    public:
        std::optional<TerrainBuiltCollider> Build(const TerrainAreaSamples &samples, float quantizationMin,
                                                  float quantizationMax) const override
        {
            ++builds;
            if (refuse)
                return std::nullopt;
            auto shape = std::make_shared<FakeShape>();
            shape->spacing = samples.spacing;
            shape->countX = samples.countX;
            shape->countZ = samples.countZ;
            shape->first = samples.samples.min;
            shape->origin = samples.origin;
            shape->quantizationMin = quantizationMin;
            shape->quantizationMax = quantizationMax;
            TerrainBuiltCollider built;
            built.shape = shape;
            built.quantizationError = 0.001;
            built.extentX = samples.spacing * (samples.countX - 1);
            built.extentZ = samples.spacing * (samples.countZ - 1);
            built.bytes = samples.heights.size() * sizeof(float);
            return built;
        }

        TerrainColliderId Place(const TerrainColliderShape &shape, WorldPosition origin,
                                const TerrainPlacement &placement) override
        {
            placed[next] = {static_cast<const FakeShape *>(&shape), origin, placement};
            return next++;
        }

        void Remove(TerrainColliderId collider) override
        {
            REQUIRE(placed.erase(collider) == 1);
            ++removed;
        }

        struct Placed
        {
            const FakeShape *shape = nullptr;
            WorldPosition origin;
            TerrainPlacement placement;
        };

        mutable std::atomic<int> builds = 0;
        std::atomic<bool> refuse = false;
        std::map<TerrainColliderId, Placed> placed;
        int removed = 0;
        TerrainColliderId next = 1;
    };

    // Rolling ground: a resample coarser than the source misses its bends by a measurable amount.
    float Ground(double x, double z)
    {
        return static_cast<float>(10.0 + 2.0 * std::sin(0.15 * x) * std::cos(0.11 * z));
    }

    // The first tiles are clipped to start on an odd sample.
    TerrainEncodedRelease CollisionRelease(const TerrainReleaseInfo &info)
    {
        const TerrainLayout layout = *TerrainLayout::CreateFromTileSize(64.0, 4.0, {-188.0, -188.0, 192.0, 192.0});
        std::vector<TileHeights> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TileHeights tile = *TileHeights::Create({x, z}, n);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        tile.Set(i, k, Ground(layout.SamplePosition(layout.TileFirstSample(x) + i),
                                              layout.SamplePosition(layout.TileFirstSample(z) + k)));
                tiles.push_back(std::move(tile));
            }
        return Encode(info, layout, tiles);
    }

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    TerrainCollisionSource Falling(const glm::dvec3 &at, double speed = 0.0, double tolerance = 0.05)
    {
        TerrainCollisionSource source;
        source.position = At(at);
        source.velocity = {0.0, -speed, 0.0};
        source.radius = 0.5;
        source.acceleration = 9.81;
        source.tolerance = tolerance;
        return source;
    }

    struct Fixture
    {
        ManualClock clock;
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        TerrainStreamingSettings settings;
        std::unique_ptr<TerrainStreamer> streamer;
        std::shared_ptr<FakePhysics> physics = std::make_shared<FakePhysics>();
        std::unique_ptr<TerrainCollisionResidency> residency;
        TerrainReleaseInfo info;
        ControlledRelease release;

        explicit Fixture(TerrainResidencyMode mode, uint64_t releaseId = 0xC011,
                         TerrainStreamingSettings::Collision collision = StreamingSettings(1).collision)
            : settings(StreamingSettings(uint64_t(1) << 30, mode)),
              info(Info(TerrainAsset("terrain/collision.terrain.json"), releaseId))
        {
            settings.collision = collision;
            streamer = std::make_unique<TerrainStreamer>(settings, executor, clock.Function());
            residency = std::make_unique<TerrainCollisionResidency>(settings.collision, executor, physics);
            release = OpenControlled(CollisionRelease(info));
            streamer->Add(release.release);
        }

        void Place(std::vector<glm::dvec3> placements)
        {
            std::vector<TerrainCollisionInstance> instances;
            for (const glm::dvec3 &at : placements)
                instances.push_back({release.release->SharedHandle(), {info.asset, At(at)},
                                     *streamer->ModeOf(info)});
            residency->SetInstances(instances);
        }

        double Now() const { return *clock.now; }

        void Step(std::span<const TerrainCollisionSource> sources)
        {
            streamer->Update();
            residency->Install(Now());
            residency->Update(sources, *streamer, Now(), kStep);
            executor->RunAll();
            clock.Advance(kStep);
        }

        void Run(int steps, std::span<const TerrainCollisionSource> sources)
        {
            for (int i = 0; i < steps; ++i)
                Step(sources);
        }

        std::set<std::pair<int32_t, int32_t>> PlacedPieces(bool coarse = false) const
        {
            std::set<std::pair<int32_t, int32_t>> pieces;
            for (const auto &[id, placed] : physics->placed)
                if ((placed.shape->spacing > 4.0) == coarse)
                    pieces.insert({int32_t(std::floor(placed.shape->origin.Meters().x / 128.0)),
                                   int32_t(std::floor(placed.shape->origin.Meters().z / 128.0))});
            return pieces;
        }
    };
}

TEST_CASE("An Eager release collides as one piece, ready only once its colliders are placed",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Eager);
    f.Place({{0.0, 0.0, 0.0}, {1000.0, 5.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({10.0, 12.5, 10.0}), Falling({1010.0, 17.5, 10.0})};

    f.residency->Update(sources, *f.streamer, f.Now(), kStep);
    CHECK(f.residency->Answer(0).status.state == State::Pending);
    CHECK(f.residency->Answer(0).status.needed == 1);
    CHECK(f.residency->Answer(0).contacts.empty());

    f.Run(4, sources);
    REQUIRE(f.physics->placed.size() == 2);
    CHECK(f.physics->builds == 2);
    std::set<double> originsX;
    for (const auto &[id, placed] : f.physics->placed)
    {
        CHECK(placed.shape == f.physics->placed.begin()->second.shape);
        originsX.insert(double(placed.origin.Meters().x));
        CHECK(placed.shape->spacing == 4.0);
    }
    CHECK(originsX == std::set<double>{-188.0, 812.0});
    for (size_t s = 0; s < sources.size(); ++s)
    {
        const TerrainCollisionAnswer &answer = f.residency->Answer(s);
        CHECK(answer.status.state == State::Ready);
        CHECK(answer.status.ready == 1);
        CHECK(answer.status.contactError <= f.settings.collision.maxErrorMeters);
        CHECK(answer.contacts.size() == 1);
        CHECK_FALSE(answer.unowned);
    }
    CHECK(f.residency->Answer(0).contacts != f.residency->Answer(1).contacts);

    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == TerrainCollisionEvent::Kind::Placed);
    CHECK(events[0].piece.whole);
    CHECK(events[0].colliders == 2);
    CHECK(std::isfinite(events[0].latencySeconds));
}

TEST_CASE("Interest pieces are built only where a source's predicted range reaches the ground",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const double reach = f.residency->PredictionSeconds();

    const std::vector<TerrainCollisionSource> high{Falling({100.0, 400.0, 100.0})};
    f.Run(10, high);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.needed == 0);
    CHECK(f.physics->builds == 0);
    CHECK(f.release.source->RangeRequests().empty());

    const std::vector<TerrainCollisionSource> diving{Falling({100.0, 400.0, 100.0}, 400.0 / reach)};
    f.Step(diving);
    CHECK(f.residency->Answer(0).status.state == State::Pending);
    CHECK(f.residency->Answer(0).status.needed == 1);
    f.Run(6, diving);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.PlacedPieces() == std::set<std::pair<int32_t, int32_t>>{{0, 0}});
    CHECK_FALSE(f.release.source->RangeRequests().empty());
    CHECK(f.residency->Answer(0).contacts.size() == 1);
}

TEST_CASE("A fast source needs the pieces ahead of it and prefetches the ones beyond", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    TerrainCollisionSource skimming = Falling({-150.0, 12.5, 100.0});
    skimming.velocity = {64.0 / f.residency->PredictionSeconds(), 0.0, 0.0};
    const std::vector<TerrainCollisionSource> sources{skimming};

    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.needed == 2);
    const std::set<std::pair<int32_t, int32_t>> placed = f.PlacedPieces();
    CHECK(placed.contains({-2, 0}));
    CHECK(placed.contains({-1, 0}));
    CHECK(placed.contains({0, 0}));
    CHECK_FALSE(placed.contains({1, 0}));
    CHECK_FALSE(placed.contains({-2, -2}));
}

TEST_CASE("A coarse piece within a source's tolerance lets it simulate while the fine piece builds",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}, 0.0, 2.0),
                                                      Falling({110.0, 13.0, 110.0}, 0.0, 0.05)};
    f.Run(2, sources);
    for (;;)
    {
        f.streamer->Update();
        f.residency->Install(f.Now());
        f.residency->Update(sources, *f.streamer, f.Now(), kStep);
        if (f.residency->Answer(0).status.state != State::Pending)
            break;
        REQUIRE(f.executor->RunOne());
        f.clock.Advance(kStep);
    }
    const TerrainCollisionAnswer &loose = f.residency->Answer(0);
    CHECK(loose.status.state == State::CoarseSufficient);
    CHECK(loose.status.coarse == 1);
    CHECK(loose.status.contactError > f.settings.collision.maxErrorMeters);
    CHECK(loose.status.contactError <= 2.0);
    CHECK(loose.contacts.size() == 1);
    CHECK(f.residency->Answer(1).status.state == State::Pending);
    CHECK(f.PlacedPieces(true) == std::set<std::pair<int32_t, int32_t>>{{0, 0}});

    f.Run(4, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(1).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.contactError <= f.settings.collision.maxErrorMeters);
    f.Run(int(f.settings.collision.retainSeconds / kStep) + 2, sources);
    CHECK(f.PlacedPieces(true).empty());
    CHECK(f.PlacedPieces() == std::set<std::pair<int32_t, int32_t>>{{0, 0}});
}

TEST_CASE("Slow and failed reads keep a source pending or failed, never ready, until the heights load",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0})};

    f.release.source->Hold();
    f.Step(sources);
    f.streamer->Update();
    for (int i = 0; i < 30; ++i)
    {
        f.residency->Install(f.Now());
        f.residency->Update(sources, *f.streamer, f.Now(), kStep);
        CHECK(f.residency->Answer(0).status.state == State::Pending);
        f.clock.Advance(kStep);
    }
    f.release.source->Resume();
    f.Run(4, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    const auto placed = std::find_if(events.begin(), events.end(), [](const TerrainCollisionEvent &e)
                                     { return e.kind == TerrainCollisionEvent::Kind::Placed && !e.piece.coarse; });
    REQUIRE(placed != events.end());
    CHECK(placed->latencySeconds >= 30 * kStep);
    CHECK(f.residency->PredictionSeconds() >=
          placed->latencySeconds + f.settings.collision.latencyMarginSeconds - 1e-9);

    Fixture failing(TerrainResidencyMode::Interest, 0xFA11);
    failing.Place({{0.0, 0.0, 0.0}});
    failing.release.source->FailNext("chunk_0_0.tpack", ~uint32_t(0));
    State seen = State::Ready;
    for (int i = 0; i < 60 && seen != State::Failed; ++i)
    {
        failing.Step(sources);
        seen = failing.residency->Answer(0).status.state;
        CHECK(seen != State::Ready);
    }
    CHECK(seen == State::Failed);
    CHECK(failing.residency->Answer(0).status.reason.starts_with("collision piece (0, 0)"));
    failing.release.source->FailNext("chunk_0_0.tpack", 0);
    failing.clock.Advance(failing.settings.retry.failedHoldSeconds);
    failing.Run(10, sources);
    CHECK(failing.residency->Answer(0).status.state == State::Ready);
}

TEST_CASE("Pieces nobody needs retire after retainSeconds, and needed pieces stay past the budget",
          "[terrain][collision]")
{
    TerrainStreamingSettings::Collision tight = StreamingSettings(1).collision;
    tight.budgetBytes = 1;
    Fixture f(TerrainResidencyMode::Interest, 0xB0D6, tight);
    f.Place({{0.0, 0.0, 0.0}});
    std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}), Falling({-150.0, 13.0, -150.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(1).status.state == State::Ready);
    CHECK(f.residency->Stats().overBudgetBytes > 0);
    const size_t before = f.physics->placed.size();

    sources.pop_back();
    f.Run(int(tight.retainSeconds / kStep) + 2, sources);
    CHECK(f.physics->placed.size() < before);
    CHECK(f.PlacedPieces().contains({0, 0}));
    CHECK_FALSE(f.PlacedPieces().contains({-2, -2}));
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const TerrainCollisionEvent &e) { return e.kind == TerrainCollisionEvent::Kind::Retired; }));
    CHECK(f.residency->Answer(0).status.state == State::Ready);
}

TEST_CASE("Sources share pieces, and a piece stays while any of them needs it", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}), Falling({110.0, 13.0, 120.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).contacts == f.residency->Answer(1).contacts);
    const int builds = f.physics->builds;

    sources.erase(sources.begin());
    f.Run(int(f.settings.collision.retainSeconds / kStep) + 2, sources);
    CHECK(f.PlacedPieces().contains({0, 0}));
    CHECK(f.physics->builds == builds);
}

TEST_CASE("A source touches the pieces that hold its footprint, and every piece near it when none does",
          "[terrain][collision]")
{
    std::vector<TerrainCollisionPieceArea> areas(2);
    areas[0].rect = {0.0, 0.0, 192.0, 128.0};
    areas[0].seams[1] = true;
    areas[0].margin = 8.0;
    areas[1].rect = {128.0, 0.0, 256.0, 128.0};
    areas[1].seams[0] = true;
    areas[1].margin = 8.0;
    bool unowned = false;
    CHECK(TerrainCollisionOwners(areas, {10.0, 10.0, 12.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK(TerrainCollisionOwners(areas, {-50.0, 10.0, -40.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK_FALSE(unowned);
    CHECK(TerrainCollisionOwners(areas, {150.0, 10.0, 152.0, 12.0}, unowned) == std::vector<size_t>{0, 1});
    CHECK(TerrainCollisionOwners(areas, {183.0, 10.0, 186.0, 12.0}, unowned) == std::vector<size_t>{1});
    CHECK(TerrainCollisionOwners(areas, {130.0, 10.0, 132.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK(TerrainCollisionOwners(areas, {300.0, -10.0, 310.0, 200.0}, unowned) == std::vector<size_t>{1});
    CHECK_FALSE(unowned);
    CHECK(TerrainCollisionOwners(areas, {120.0, 10.0, 200.0, 12.0}, unowned) == std::vector<size_t>{0, 1});
    CHECK(unowned);

    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> band{Falling({160.0, 13.0, 100.0})};
    f.Run(8, band);
    CHECK(f.residency->Answer(0).contacts.size() == 2);
    CHECK_FALSE(f.residency->Answer(0).unowned);
    const std::vector<TerrainCollisionSource> core{Falling({100.0, 13.0, 100.0})};
    f.Run(2, core);
    CHECK(f.residency->Answer(0).contacts.size() == 1);
}

TEST_CASE("Pieces start on an even sample and share one quantization range", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({-150.0, 13.0, -150.0}), Falling({100.0, 13.0, 100.0}),
                                                      Falling({170.0, 13.0, 170.0})};
    f.Run(8, sources);
    REQUIRE(f.physics->placed.size() >= 3);
    CHECK(f.PlacedPieces().contains({-2, -2}));
    const FakeShape *first = f.physics->placed.begin()->second.shape;
    for (const auto &[id, placed] : f.physics->placed)
    {
        CHECK(placed.shape->first.x % 2 == 0);
        CHECK(placed.shape->first.z % 2 == 0);
        CHECK(placed.shape->quantizationMin == first->quantizationMin);
        CHECK(placed.shape->quantizationMax == first->quantizationMax);
    }
}

TEST_CASE("Changing the instances drops the colliders of placements and releases that left", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Eager);
    f.Place({{0.0, 0.0, 0.0}, {1000.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> none;
    f.Run(4, none);
    CHECK(f.physics->placed.size() == 2);

    f.Place({{0.0, 0.0, 0.0}});
    CHECK(f.physics->placed.size() == 1);
    f.residency->SetInstances({});
    CHECK(f.physics->placed.empty());
    f.Run(4, none);
    CHECK(f.physics->placed.empty());

    f.Place({{0.0, 0.0, 0.0}});
    f.residency->Update(none, *f.streamer, f.Now(), kStep);
    f.residency->SetInstances({});
    f.executor->RunAll();
    f.Run(2, none);
    CHECK(f.physics->placed.empty());
}

TEST_CASE("A piece whose heights make no shape fails once and the source never reads ready", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    f.physics->refuse = true;
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Failed);
    CHECK(f.residency->Answer(0).status.reason.find("no height field") != std::string::npos);
    const int builds = f.physics->builds;
    f.Run(8, sources);
    CHECK(f.physics->builds == builds);
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    CHECK(std::count_if(events.begin(), events.end(), [](const TerrainCollisionEvent &e)
                        { return e.kind == TerrainCollisionEvent::Kind::Failed; }) >= 1);
}
