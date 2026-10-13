/**
 * @file TerrainTelemetry.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainTelemetry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainTelemetry.h>
#include <terrain/TerrainProfiledSource.h>
#include <terrain/TerrainScene.h>

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"
#include "TerrainResidencyTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <thread>

using namespace mir;

TEST_CASE("Terrain telemetry merges requests and keeps each capability and release separate", "[terrain][telemetry]")
{
    double now = 0.0;
    TerrainTelemetry telemetry([&] { return now; });
    const TerrainReleaseInfo a{.asset = AssetId{1}, .release = TerrainReleaseId{2}};
    const TerrainReleaseInfo b{.asset = AssetId{1}, .release = TerrainReleaseId{3}};
    telemetry.Request("queryable", "A", a);
    now = 0.25;
    telemetry.Request("queryable", "A", a);
    telemetry.Request("renderable", "A", a);
    telemetry.Request("queryable", "B", b);
    now = 0.5;
    telemetry.Complete("queryable", "A");
    telemetry.Complete("queryable", "A");
    const auto samples = telemetry.TakeSamples();
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].seconds == 0.5);
    CHECK(samples[0].release == a);
    CHECK(samples[0].success);
    CHECK(telemetry.Pending().at("queryable") == 1);
    CHECK(telemetry.Pending().at("renderable") == 1);
    now = 1.0;
    telemetry.Complete("queryable", "B");
    const auto next = telemetry.TakeSamples();
    REQUIRE(next.size() == 1);
    CHECK(next[0].release == b);
    CHECK(next[0].seconds == 0.75);
}

TEST_CASE("Canceled and outstanding terrain requests are not successful latency samples", "[terrain][telemetry]")
{
    double now = 0.0;
    TerrainTelemetry telemetry([&] { return now; });
    telemetry.Request("renderable", "canceled", {});
    telemetry.Request("renderable", "outstanding", {});
    now = 0.5;
    telemetry.Cancel("renderable", "canceled");
    telemetry.Record("invalid", std::numeric_limits<double>::quiet_NaN());
    telemetry.Record("invalid", -1.0);
    const auto samples = telemetry.TakeSamples();
    REQUIRE(samples.size() == 1);
    CHECK_FALSE(samples[0].success);
    CHECK(samples[0].seconds == 0.5);
    CHECK(telemetry.Pending().at("renderable") == 1);
    CHECK(telemetry.TakeSamples().empty());
}

TEST_CASE("Terrain telemetry accepts concurrent worker samples without losing the owner thread", "[terrain][telemetry]")
{
    TerrainTelemetry telemetry;
    REQUIRE(telemetry.OnOwnerThread());
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i)
        workers.emplace_back([&]
        {
            for (int j = 0; j < 100; ++j)
                telemetry.Record(telemetry.OnOwnerThread() ? "wrong-thread" : "worker", 0.01);
        });
    workers.clear();
    const auto samples = telemetry.TakeSamples();
    REQUIRE(samples.size() == 400);
    CHECK(std::all_of(samples.begin(), samples.end(), [](const auto &sample) { return sample.metric == "worker"; }));
}

TEST_CASE("Profiled terrain range failures recover and detect main thread reads", "[terrain][telemetry]")
{
    auto source = std::make_shared<TerrainMemorySource>();
    source->Put("pack", std::vector<std::byte>(8, std::byte{4}));
    auto telemetry = std::make_shared<TerrainTelemetry>();
    auto faults = std::make_shared<TerrainReadFaults>();
    TerrainProfiledSource measured(source, telemetry, faults, {});
    faults->remaining = 1;
    CHECK_FALSE(measured.ReadRange("pack", 0, 4));
    REQUIRE(measured.ReadRange("pack", 0, 4));
    CHECK(faults->injected == 1);
    CHECK(faults->mainThreadRanges == 2);
    CHECK(source->Stats().reads == 1);
    std::jthread worker([&] { (void)measured.ReadRange("pack", 0, 4); });
    worker.join();
    CHECK(faults->mainThreadRanges == 2);
    const auto samples = telemetry->TakeSamples();
    REQUIRE(samples.size() == 3);
    CHECK_FALSE(samples[0].success);
    CHECK(samples[1].success);
    CHECK(samples[2].metric == "io_range");
}

TEST_CASE("Terrain query readiness is sampled at the install point after merged reads", "[terrain][telemetry]")
{
    auto opened = test::OpenInMemory(test::EncodeEdge(AssetId{6}, 7));
    auto release = TerrainResidentRelease::Create(std::move(opened.handle));
    auto executor = std::make_shared<TerrainManualExecutor>();
    double now = 0.0;
    auto telemetry = std::make_shared<TerrainTelemetry>([&] { return now; });
    TerrainStreamer streamer(test::StreamingSettings(1 << 20, TerrainResidencyMode::Interest), executor);
    streamer.SetTelemetry(telemetry);
    streamer.Add(release);
    const auto &record = release->Handle().Index().tiles.front();
    const TerrainPayloadWant want{TerrainPayloadKeyOf(release->Info(), record), TerrainDemand::Query, 0.0f};
    const auto first = streamer.AddInterest(), second = streamer.AddInterest();
    streamer.SetInterest(first, std::span(&want, 1));
    streamer.SetInterest(second, std::span(&want, 1));
    streamer.Update();
    executor->RunAll();
    CHECK(telemetry->TakeSamples().empty());
    now = 0.25;
    streamer.Update();
    const auto samples = telemetry->TakeSamples();
    const auto ready = std::find_if(samples.begin(), samples.end(), [](const auto &sample) { return sample.metric == "queryable"; });
    REQUIRE(ready != samples.end());
    CHECK(ready->seconds == 0.25);
    CHECK(ready->release == release->Info());
    CHECK(streamer.Stats().readsStarted == 1);
    CHECK(telemetry->Pending().empty());
    const auto held = streamer.Heights(want.key);
    REQUIRE(held);
    streamer.EvictPayloads();
    CHECK(streamer.Status(want.key).state == TerrainPayloadState::Absent);
    CHECK(streamer.Stats().residentBytes == 0);
    CHECK(held->Tile() == want.key.coord);
    streamer.Update();
    executor->RunAll();
    now = 0.5;
    streamer.Update();
    CHECK(streamer.Stats().readsStarted == 2);
    CHECK(streamer.Status(want.key).state == TerrainPayloadState::Ready);
}

TEST_CASE("Terrain collision profiling keeps pending work until a collider is placed or canceled", "[terrain][telemetry]")
{
    auto opened = test::OpenInMemory(test::EncodeEdge(AssetId{8}, 9));
    auto release = TerrainResidentRelease::Create(std::move(opened.handle));
    auto executor = std::make_shared<TerrainManualExecutor>();
    const auto settings = test::StreamingSettings(1 << 20, TerrainResidencyMode::Eager);
    TerrainStreamer streamer(settings, executor);
    streamer.Add(release);
    streamer.Update();
    executor->RunAll();
    streamer.Update();
    double now = 0.0;
    auto telemetry = std::make_shared<TerrainTelemetry>([&] { return now; });
    auto physics = std::make_shared<test::FakePhysics>();
    TerrainCollisionResidency collision(settings.collision, executor, physics);
    collision.SetTelemetry(telemetry);
    const TerrainCollisionInstance instance{release->SharedHandle(), {release->Info().asset, WorldPosition{}}, TerrainResidencyMode::Eager};
    collision.SetInstances(std::span(&instance, 1));
    collision.Update({}, streamer, now, 0.0);
    REQUIRE(telemetry->Pending().at("collidable") == 1);
    executor->RunAll();
    CHECK(telemetry->TakeSamples().empty());
    now = 0.25;
    collision.Install(now);
    const auto samples = telemetry->TakeSamples();
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].success);
    CHECK(samples[0].seconds == 0.25);
    CHECK(samples[0].release.release == release->Info().release);
    CHECK(telemetry->Pending().empty());
    collision.Clear();
    collision.SetInstances(std::span(&instance, 1));
    collision.Update({}, streamer, now, 0.0);
    REQUIRE(telemetry->Pending().at("collidable") == 1);
    now = 0.5;
    collision.Clear();
    const auto canceled = telemetry->TakeSamples();
    REQUIRE(canceled.size() == 1);
    CHECK_FALSE(canceled[0].success);
    CHECK(telemetry->Pending().empty());
}
