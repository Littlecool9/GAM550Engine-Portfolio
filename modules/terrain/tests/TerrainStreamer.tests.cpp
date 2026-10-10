/**
 * @file TerrainStreamer.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainStreamer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;
    using Demand = TerrainDemand;
    using Mode = TerrainResidencyMode;
    using State = TerrainPayloadState;
    using Status = TerrainHeightStatus;

    struct Streamed
    {
        TerrainLayout layout;
        std::vector<TileHeights> tiles;
        TerrainReleaseInfo info;
        TerrainEncodedRelease encoded;
        ControlledRelease opened;

        explicit Streamed(uint64_t release = 0x126, TerrainLayout l = ReliefLayout(), bool regions = true)
            : layout(l), tiles(BuildTiles(l)), info(Info(TerrainAsset("terrain/streamed.terrain.json"), release)),
              encoded(regions ? EncodeWithRegions(info, l, tiles, BuildTestRegions(l, tiles, 1, 8))
                              : Encode(info, l, tiles)),
              opened(OpenControlled(encoded))
        {
        }

        const TerrainIndex &Index() const { return opened.release->Handle().Index(); }
        TerrainHeightQuery &Query() { return opened.release->Query(); }

        TerrainPayloadKey Tile(TileCoord tile) const
        {
            const TerrainTileRecord *record = Index().Find(tile);
            REQUIRE(record);
            return TerrainPayloadKeyOf(info, *record);
        }

        TerrainPayloadKey Region(TerrainRegionCoord region) const
        {
            const TerrainTileRecord *record = Index().FindRegion(region);
            REQUIRE(record);
            return TerrainPayloadKeyOf(info, *record);
        }

        const TerrainTileRecord &Record(const TerrainPayloadKey &key) const
        {
            const TerrainTileRecord *record = Index().Find(key.coord, key.layer, key.kind, key.level);
            REQUIRE(record);
            return *record;
        }

        std::string Pack(const TerrainPayloadKey &key) const
        {
            return TerrainPackFileName(Index().packs[Record(key).pack].chunk);
        }

        const TileHeights &Written(TileCoord tile) const
        {
            const auto it = std::find_if(tiles.begin(), tiles.end(), [&](const TileHeights &t) { return t.Tile() == tile; });
            REQUIRE(it != tiles.end());
            return *it;
        }

        // A point inside the tile.
        TerrainLocalPosition In(TileCoord tile) const
        {
            const double x = layout.SamplePosition(layout.TileFirstSample(tile.x) + 3) + 0.25;
            const double z = layout.SamplePosition(layout.TileFirstSample(tile.z) + 5) + 0.5;
            const auto local = TerrainLocalPosition::FromMeters({std::clamp(x, layout.Bounds().minX, layout.Bounds().maxX),
                                                                 0.0,
                                                                 std::clamp(z, layout.Bounds().minZ, layout.Bounds().maxZ)});
            REQUIRE(local);
            return *local;
        }

        TerrainHeightResult HeightAt(TileCoord tile, double spacing = 4.0)
        {
            return Query().HeightAt(In(tile), {info, spacing});
        }

        double Expected(TileCoord tile) const
        {
            const SurfaceSample sample = EvaluateSurface(layout, In(tile), [this](TileCoord owner) {
                return Written(owner).Samples();
            });
            REQUIRE(sample.IsOk());
            return sample.height;
        }
    };

    struct Manual
    {
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        ManualClock clock;
        TerrainStreamer streamer;

        explicit Manual(TerrainStreamingSettings settings) : streamer(settings, executor, clock.Function()) {}

        // Runs every posted read, then installs them.
        void Settle()
        {
            streamer.Update();
            executor->RunAll();
            streamer.Update();
        }
    };

    void Want(TerrainStreamer &streamer, TerrainInterestId interest, std::vector<TerrainPayloadWant> wants)
    {
        streamer.SetInterest(interest, wants);
    }

    bool SameRange(const TerrainMemorySource::Request &request, const std::string &pack, const TerrainTileRecord &record)
    {
        return request.file == pack && request.offset == record.offset && request.length == record.length;
    }

    void CheckWithinBudget(const TerrainStreamer &streamer)
    {
        const TerrainStreamingStats stats = streamer.Stats();
        CHECK(stats.residentBytes + stats.loadingBytes <= stats.budgetBytes);
    }
}

TEST_CASE("Repeated wants for one payload share one range read and one decoded payload", "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    REQUIRE(m.streamer.Add(s.opened.release) == Mode::Interest);
    const TerrainPayloadKey tile = s.Tile({0, 0});
    const TerrainPayloadKey region = s.Region({0, 0, 0});
    const size_t opened = s.opened.source->Inner().Requests().size();

    const TerrainInterestId viewA = m.streamer.AddInterest();
    const TerrainInterestId viewB = m.streamer.AddInterest();
    Want(m.streamer, viewA, {{tile, Demand::Visible, 2.0f}, {region, Demand::Prefetch, 1.0f}});
    Want(m.streamer, viewB, {{tile, Demand::Required, 0.0f}, {region, Demand::Visible, 0.0f}});
    CHECK(s.HeightAt({0, 0}).status == Status::Loading);

    m.streamer.Update();
    CHECK(m.streamer.Status(tile).state == State::Loading);
    CHECK(m.streamer.Status(region).state == State::Loading);
    CHECK(m.executor->Pending() == 2);
    CHECK(s.opened.source->RangeRequests().empty());
    CHECK(s.HeightAt({0, 0}).status == Status::Loading);

    m.executor->RunAll();
    const std::vector<TerrainMemorySource::Request> ranges = s.opened.source->RangeRequests();
    REQUIRE(ranges.size() == 2);
    CHECK(std::any_of(ranges.begin(), ranges.end(),
                      [&](const auto &r) { return SameRange(r, s.Pack(tile), s.Record(tile)); }));
    CHECK(std::any_of(ranges.begin(), ranges.end(),
                      [&](const auto &r) { return SameRange(r, s.Pack(region), s.Record(region)); }));
    CHECK(s.opened.source->Inner().Requests().size() == opened + 2);

    m.streamer.Update();
    CHECK(m.streamer.Status(tile).state == State::Ready);
    CHECK(m.streamer.Status(region).state == State::Ready);
    const std::shared_ptr<const TileHeights> heights = m.streamer.Heights(tile);
    REQUIRE(heights);
    CHECK(heights == s.Query().ResidentTile(0, {0, 0}));
    CHECK(std::ranges::equal(heights->Samples(), s.Written({0, 0}).Samples()));
    const std::shared_ptr<const TerrainRegionMesh> mesh = m.streamer.Region(region);
    REQUIRE(mesh);
    CHECK(mesh->region == TerrainRegionCoord{0, 0, 0});
    CHECK(m.streamer.Heights(region) == nullptr);
    CHECK(m.streamer.Region(tile) == nullptr);

    const TerrainHeightResult r = s.HeightAt({0, 0});
    REQUIRE(r.status == Status::MeetsRequirement);
    CHECK(r.localHeight == s.Expected({0, 0}));
    m.streamer.Update();
    CHECK(m.streamer.Stats().readsStarted == 2);
    CHECK(s.opened.source->RangeRequests().size() == 2);
}

TEST_CASE("A want dropped before its read starts is canceled while other consumers keep theirs",
          "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);
    const TerrainPayloadKey first = s.Tile({-1, -1});
    const TerrainPayloadKey shared = s.Tile({0, -1});
    const TerrainInterestId a = m.streamer.AddInterest();
    const TerrainInterestId b = m.streamer.AddInterest();
    Want(m.streamer, a, {{first, Demand::Visible}, {shared, Demand::Visible}});
    Want(m.streamer, b, {{shared, Demand::Prefetch}});
    m.streamer.Update();
    REQUIRE(m.executor->Pending() == 2);

    Want(m.streamer, a, {});
    m.streamer.Update();
    CHECK(m.streamer.Status(first).state == State::Absent);
    CHECK(m.streamer.Status(shared).state == State::Loading);
    CHECK(m.streamer.Stats().canceled == 1);
    m.executor->RunAll();
    m.streamer.Update();
    REQUIRE(s.opened.source->RangeRequests().size() == 1);
    CHECK(SameRange(s.opened.source->RangeRequests()[0], s.Pack(shared), s.Record(shared)));
    CHECK(m.streamer.Status(shared).state == State::Ready);

    SECTION("A read already started finishes and its payload stays until evicted")
    {
        s.opened.source->Hold();
        Want(m.streamer, a, {{first, Demand::Visible}});
        m.streamer.Update();
        std::thread worker([&] { m.executor->RunOne(); });
        while (s.opened.source->BlockedReads() == 0)
            std::this_thread::yield();
        Want(m.streamer, a, {});
        m.streamer.Update();
        CHECK(m.streamer.Status(first).state == State::Loading);
        s.opened.source->Resume();
        worker.join();
        m.streamer.Update();
        CHECK(m.streamer.Status(first).state == State::Ready);
        CHECK(m.streamer.Stats().canceled == 1);
        CHECK(m.streamer.Stats().discarded == 0);
    }
    SECTION("Removing an interest keeps what another one still wants, without reading it again")
    {
        Want(m.streamer, a, {{shared, Demand::Visible}});
        m.streamer.RemoveInterest(b);
        m.streamer.Update();
        CHECK(m.streamer.Status(shared).state == State::Ready);
        const std::shared_ptr<const TileHeights> heights = m.streamer.Heights(shared);
        m.streamer.RemoveInterest(a);
        m.streamer.Update();
        CHECK(m.streamer.Heights(shared) == heights);
        CHECK(m.streamer.Stats().readsStarted == 2);
    }
}

TEST_CASE("Reads start in demand order, then rank, and follow a raised demand", "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);
    const TerrainPayloadKey prefetch = s.Tile({-1, -1});
    const TerrainPayloadKey farVisible = s.Tile({0, -1});
    const TerrainPayloadKey nearVisible = s.Tile({-1, 0});
    const TerrainPayloadKey query = s.Tile({0, 0});
    const TerrainPayloadKey root = s.Region({1, 0, 0});
    const TerrainInterestId view = m.streamer.AddInterest();
    Want(m.streamer, view, {{prefetch, Demand::Prefetch, 0.0f},
                            {farVisible, Demand::Visible, 5.0f},
                            {nearVisible, Demand::Visible, 1.0f},
                            {query, Demand::Query, 9.0f},
                            {root, Demand::Required, 3.0f}});
    m.streamer.Update();
    REQUIRE(m.executor->Pending() == 5);

    Want(m.streamer, view, {{prefetch, Demand::Required, 0.0f},
                            {farVisible, Demand::Visible, 5.0f},
                            {nearVisible, Demand::Visible, 1.0f},
                            {query, Demand::Query, 9.0f},
                            {root, Demand::Required, 3.0f}});
    m.streamer.Update();
    m.executor->RunAll();
    const std::vector<TerrainMemorySource::Request> ranges = s.opened.source->RangeRequests();
    REQUIRE(ranges.size() == 5);
    const TerrainPayloadKey expected[]{prefetch, root, query, nearVisible, farVisible};
    for (size_t i = 0; i < 5; ++i)
    {
        INFO("read " << i);
        CHECK(SameRange(ranges[i], s.Pack(expected[i]), s.Record(expected[i])));
    }
}

TEST_CASE("Corrupt ranges fail at once, truncated ones after the retry policy, and neither reads as height zero",
          "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);

    const TerrainPayloadKey corrupt = s.Tile({0, 0});
    s.opened.source->Corrupt(s.Pack(corrupt), s.Record(corrupt).offset + 7);
    CHECK(s.HeightAt({0, 0}).status == Status::Loading);
    m.Settle();
    const TerrainPayloadStatus failed = m.streamer.Status(corrupt);
    CHECK(failed.state == State::Failed);
    CHECK(failed.error.code == Code::Corrupt);
    CHECK(failed.attempts == 1);
    CHECK(Mentions(failed.error, "checksum"));
    CHECK(Mentions(failed.error, "Height tile (0, 0)"));
    CHECK(m.streamer.Stats().retries == 0);
    const TerrainHeightResult r = s.HeightAt({0, 0});
    CHECK(r.status == Status::Failed);
    CHECK(r.failure == TerrainHeightFailure::LoadFailed);
    CHECK_FALSE(r.HasHeight());
    CHECK(std::isnan(r.localHeight));

    const TerrainPayloadKey truncated = s.Tile({-1, -1});
    const std::string pack = s.Pack(truncated);
    std::optional<std::vector<std::byte>> bytes = s.opened.source->Inner().Get(pack);
    REQUIRE(bytes);
    bytes->resize(size_t(s.Record(truncated).offset + s.Record(truncated).length - 1));
    s.opened.source->Inner().Put(pack, *bytes);

    CHECK(s.HeightAt({-1, -1}).status == Status::Loading);
    m.Settle();
    CHECK(m.streamer.Status(truncated).state == State::Waiting);
    CHECK(m.streamer.Status(truncated).attempts == 1);
    CHECK(s.HeightAt({-1, -1}).status == Status::Loading);
    m.streamer.Update();
    CHECK(m.executor->Pending() == 0);

    m.clock.Advance(0.001);
    m.Settle();
    CHECK(m.streamer.Status(truncated).attempts == 2);
    CHECK(m.streamer.Status(truncated).state == State::Waiting);
    m.clock.Advance(0.002);
    m.Settle();
    const TerrainPayloadStatus gaveUp = m.streamer.Status(truncated);
    CHECK(gaveUp.state == State::Failed);
    CHECK(gaveUp.error.code == Code::Io);
    CHECK(gaveUp.attempts == 3);
    CHECK(m.streamer.Stats().retries == 2);
    CHECK(s.HeightAt({-1, -1}).failure == TerrainHeightFailure::LoadFailed);
    CHECK(std::isnan(s.HeightAt({-1, -1}).localHeight));
}

TEST_CASE("A failed read recovers under the retry policy, after its hold, or when retried explicitly",
          "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);
    const TerrainInterestId view = m.streamer.AddInterest();

    const TerrainPayloadKey flaky = s.Tile({0, 0});
    s.opened.source->FailNext(s.Pack(flaky), 2);
    Want(m.streamer, view, {{flaky, Demand::Visible}});
    m.Settle();
    m.clock.Advance(0.001);
    m.Settle();
    m.clock.Advance(0.002);
    m.Settle();
    CHECK(m.streamer.Status(flaky).state == State::Ready);
    CHECK(m.streamer.Stats().retries == 2);
    CHECK(s.HeightAt({0, 0}).status == Status::MeetsRequirement);

    const TerrainPayloadKey held = s.Tile({-1, 0});
    s.opened.source->FailNext(s.Pack(held), 3);
    Want(m.streamer, view, {{held, Demand::Visible}});
    for (double delay : {0.0, 0.001, 0.002})
    {
        m.clock.Advance(delay);
        m.Settle();
    }
    REQUIRE(m.streamer.Status(held).state == State::Failed);
    const size_t reads = s.opened.source->RangeRequests().size();
    m.clock.Advance(0.5);
    m.Settle();
    CHECK(m.streamer.Status(held).state == State::Failed);
    CHECK(s.opened.source->RangeRequests().size() == reads);
    m.clock.Advance(0.6);
    m.Settle();
    CHECK(m.streamer.Status(held).state == State::Ready);
    CHECK(s.HeightAt({-1, 0}).status == Status::MeetsRequirement);

    const TerrainPayloadKey retried = s.Tile({0, -1});
    s.opened.source->FailNext(s.Pack(retried), 3);
    Want(m.streamer, view, {{retried, Demand::Visible}});
    for (double delay : {0.0, 0.001, 0.002})
    {
        m.clock.Advance(delay);
        m.Settle();
    }
    REQUIRE(m.streamer.Status(retried).state == State::Failed);
    m.streamer.RetryFailed();
    m.Settle();
    CHECK(m.streamer.Status(retried).state == State::Ready);
}

TEST_CASE("CPU residency stays within the budget and speculative data goes before query and required data",
          "[terrain][streaming]")
{
    const auto layout = TerrainLayout::CreateFromTileSize(64.0, 4.0, {0.0, 0.0, 256.0, 256.0});
    REQUIRE(layout);
    Streamed s(0x1260, *layout, false);
    const uint64_t each = TerrainPayloadFootprint(s.Record(s.Tile({0, 0})));
    Manual m(StreamingSettings(6 * each, Mode::Interest));
    m.streamer.Add(s.opened.release);
    const auto tile = [&](int i) { return s.Tile({i % 4, i / 4}); };
    const auto settle = [&]
    {
        m.streamer.Update();
        CheckWithinBudget(m.streamer);
        m.executor->RunAll();
        m.streamer.Update();
        CheckWithinBudget(m.streamer);
    };

    const TerrainInterestId prefetch = m.streamer.AddInterest();
    std::vector<TerrainPayloadWant> wants;
    for (int i = 0; i < 6; ++i)
        wants.push_back({tile(i), Demand::Prefetch, float(i)});
    Want(m.streamer, prefetch, wants);
    settle();
    for (int i = 0; i < 6; ++i)
        CHECK(m.streamer.Status(tile(i)).state == State::Ready);
    const std::shared_ptr<const TileHeights> kept = m.streamer.Heights(tile(5));
    REQUIRE(kept);

    const TerrainInterestId visible = m.streamer.AddInterest();
    Want(m.streamer, visible, {{tile(6), Demand::Visible, 0.0f}, {tile(7), Demand::Visible, 1.0f}});
    settle();
    CHECK(m.streamer.Status(tile(6)).state == State::Ready);
    CHECK(m.streamer.Status(tile(7)).state == State::Ready);
    CHECK(m.streamer.Status(tile(5)).state == State::Waiting);
    CHECK(m.streamer.Status(tile(4)).state == State::Waiting);
    CHECK(m.streamer.Status(tile(3)).state == State::Ready);
    CHECK(std::ranges::equal(kept->Samples(), s.Written({1, 1}).Samples()));

    CHECK(s.HeightAt({0, 2}).status == Status::Loading);
    settle();
    CHECK(m.streamer.Status(tile(8)).state == State::Ready);
    CHECK(m.streamer.Status(tile(3)).state == State::Waiting);

    const TerrainInterestId collision = m.streamer.AddInterest();
    Want(m.streamer, collision, {{tile(9), Demand::Required}, {tile(10), Demand::Required},
                                 {tile(11), Demand::Required}, {tile(12), Demand::Required}});
    settle();
    for (int i = 9; i <= 12; ++i)
        CHECK(m.streamer.Status(tile(i)).state == State::Ready);
    CHECK(m.streamer.Status(tile(8)).state == State::Ready);
    CHECK(m.streamer.Status(tile(6)).state == State::Ready);
    CHECK(m.streamer.Status(tile(7)).state == State::Waiting);
    for (int i = 0; i < 6; ++i)
        CHECK(m.streamer.Status(tile(i)).state == State::Waiting);
    CHECK(m.streamer.Stats().overBudgetBytes == 0);
    CHECK(m.streamer.Stats().peakBytes <= 6 * each);

    SECTION("Required data alone may pass the budget, and only by what it needs")
    {
        std::vector<TerrainPayloadWant> required;
        for (int i = 9; i <= 15; ++i)
            required.push_back({tile(i), Demand::Required});
        Want(m.streamer, collision, required);
        m.streamer.Update();
        m.executor->RunAll();
        m.streamer.Update();
        for (int i = 9; i <= 15; ++i)
            CHECK(m.streamer.Status(tile(i)).state == State::Ready);
        CHECK(m.streamer.Stats().overBudgetBytes == each);
        CHECK(m.streamer.Status(tile(8)).state == State::Waiting);
        CHECK(s.Query().Residency(0, {0, 2}) == TerrainTileResidency::Absent);
        CHECK(s.HeightAt({0, 2}).status == Status::Loading);
    }
}

TEST_CASE("Auto residency is Eager only when every payload fits what the budget has left", "[terrain][streaming]")
{
    Streamed a(0xA126);
    Streamed b(0xB126);
    uint64_t total = 0;
    for (const auto *records : {&a.Index().tiles, &a.Index().regions})
        for (const TerrainTileRecord &record : *records)
            total += TerrainPayloadFootprint(record);

    Manual m(StreamingSettings(total, Mode::Auto));
    CHECK(m.streamer.Add(a.opened.release) == Mode::Eager);
    CHECK(m.streamer.Add(a.opened.release) == Mode::Eager);
    CHECK(m.streamer.Add(b.opened.release) == Mode::Interest);
    CHECK(m.streamer.ModeOf(a.info) == Mode::Eager);
    CHECK(m.streamer.ModeOf(b.info) == Mode::Interest);
    CHECK(m.streamer.Find(a.info.asset, a.info.release) == a.opened.release);
    CHECK_FALSE(m.streamer.ModeOf(Info(a.info.asset, 0x999)));

    m.Settle();
    for (const auto *records : {&a.Index().tiles, &a.Index().regions})
        for (const TerrainTileRecord &record : *records)
        {
            const TerrainPayloadKey key = TerrainPayloadKeyOf(a.info, record);
            INFO(DescribeTerrainPayloadKey(key));
            CHECK(m.streamer.Status(key).state == State::Ready);
        }
    CHECK(m.streamer.Stats().residentBytes == total);
    CHECK(m.streamer.Stats().readsStarted == a.Index().tiles.size() + a.Index().regions.size());
    CHECK(b.opened.source->RangeRequests().empty());

    const std::shared_ptr<const TileHeights> heights = m.streamer.Heights(a.Tile({0, 0}));
    REQUIRE(heights);
    CHECK(TerrainPayloadFootprint(a.Record(a.Tile({0, 0}))) ==
          sizeof(TileHeights) + heights->Samples().size() * sizeof(float));
    const std::shared_ptr<const TerrainRegionMesh> mesh = m.streamer.Region(a.Region({0, 0, 0}));
    REQUIRE(mesh);
    CHECK(TerrainPayloadFootprint(a.Record(a.Region({0, 0, 0}))) ==
          sizeof(TerrainRegionMesh) + (mesh->positions.size() + mesh->normals.size()) * sizeof(glm::vec3) +
              mesh->indices.size() * sizeof(uint32_t));
    const size_t reads = a.opened.source->RangeRequests().size();
    for (const TileCoord tile : {TileCoord{-1, -1}, TileCoord{0, -1}, TileCoord{-1, 0}, TileCoord{0, 0}})
        CHECK(a.HeightAt(tile).status == Status::MeetsRequirement);
    m.Settle();
    CHECK(a.opened.source->RangeRequests().size() == reads);

    Manual eager(StreamingSettings(1, Mode::Eager));
    CHECK(eager.streamer.Add(b.opened.release) == Mode::Eager);
    Manual interest(StreamingSettings(uint64_t(1) << 30, Mode::Interest));
    CHECK(interest.streamer.Add(a.opened.release) == Mode::Interest);
}

TEST_CASE("An Eager release loads through the worker pool and the wait reports the first failure",
          "[terrain][streaming]")
{
    Streamed good(0xE126);
    TerrainStreamer streamer(StreamingSettings(uint64_t(1) << 30, Mode::Eager), std::make_shared<TerrainWorkerPool>(2));
    REQUIRE(streamer.Add(good.opened.release) == Mode::Eager);
    const TerrainPackError loaded = streamer.WaitForEager(good.info);
    INFO(loaded.message);
    CHECK(loaded.code == Code::None);
    CHECK_FALSE(good.opened.source->ReaderThreads().contains(std::this_thread::get_id()));
    CHECK(good.opened.source->RangeRequests().size() == good.Index().tiles.size() + good.Index().regions.size());
    const size_t reads = good.opened.source->RangeRequests().size();
    for (const TileCoord tile : {TileCoord{-1, -1}, TileCoord{0, -1}, TileCoord{-1, 0}, TileCoord{0, 0}})
    {
        const TerrainHeightResult r = good.HeightAt(tile);
        REQUIRE(r.status == Status::MeetsRequirement);
        CHECK(r.localHeight == good.Expected(tile));
    }
    CHECK(good.opened.source->RangeRequests().size() == reads);

    Streamed bad(0xBAD);
    const TerrainPayloadKey broken = bad.Region({1, 0, 0});
    bad.opened.source->Corrupt(bad.Pack(broken), bad.Record(broken).offset + 3);
    REQUIRE(streamer.Add(bad.opened.release) == Mode::Eager);
    const TerrainPackError refused = streamer.WaitForEager(bad.info);
    CHECK(refused.code == Code::Corrupt);
    CHECK(Mentions(refused, "Region L1 (0, 0)"));

    TerrainStreamer lazy(StreamingSettings(uint64_t(1) << 30, Mode::Interest), std::make_shared<TerrainWorkerPool>(1));
    lazy.Add(good.opened.release);
    CHECK(lazy.WaitForEager(good.info).code == Code::InvalidInput);
}

TEST_CASE("Height-query tiles stay wanted while used and become evictable when their lease runs out",
          "[terrain][streaming]")
{
    Streamed s(0x1EA5, ReliefLayout(), false);
    const uint64_t each = TerrainPayloadFootprint(s.Record(s.Tile({0, 0})));
    Manual m(StreamingSettings(each, Mode::Interest));
    m.streamer.Add(s.opened.release);
    CHECK(s.HeightAt({0, 0}).status == Status::Loading);
    m.Settle();
    REQUIRE(s.HeightAt({0, 0}).status == Status::MeetsRequirement);

    const TerrainInterestId view = m.streamer.AddInterest();
    Want(m.streamer, view, {{s.Tile({-1, 0}), Demand::Visible}});
    for (int frame = 0; frame < 4; ++frame)
    {
        m.clock.Advance(0.6);
        CHECK(s.HeightAt({0, 0}).status == Status::MeetsRequirement);
        m.Settle();
        CHECK(m.streamer.Status(s.Tile({0, 0})).state == State::Ready);
        CHECK(m.streamer.Status(s.Tile({-1, 0})).state == State::Waiting);
    }

    m.clock.Advance(1.5);
    m.Settle();
    CHECK(m.streamer.Status(s.Tile({-1, 0})).state == State::Ready);
    CHECK(m.streamer.Status(s.Tile({0, 0})).state == State::Absent);
    CHECK(s.Query().Residency(0, {0, 0}) == TerrainTileResidency::Absent);
    const TerrainHeightResult r = s.HeightAt({0, 0});
    CHECK(r.status == Status::Loading);
    CHECK(std::isnan(r.localHeight));
    CHECK(m.streamer.Stats().evicted == 1);
}

TEST_CASE("Dropping a release drops its payloads and discards its unfinished reads", "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);
    const TerrainInterestId view = m.streamer.AddInterest();
    const TerrainPayloadKey ready = s.Tile({0, 0});
    Want(m.streamer, view, {{ready, Demand::Visible}});
    m.Settle();
    REQUIRE(m.streamer.Status(ready).state == State::Ready);

    std::vector<TerrainPayloadWant> wants{{ready, Demand::Visible}};
    for (const TileCoord tile : {TileCoord{-1, -1}, TileCoord{0, -1}, TileCoord{-1, 0}})
        wants.push_back({s.Tile(tile), Demand::Visible});
    Want(m.streamer, view, wants);
    s.opened.source->Hold();
    m.streamer.Update();
    REQUIRE(m.executor->Pending() == 3);
    std::thread worker([&] { m.executor->RunOne(); });
    while (s.opened.source->BlockedReads() == 0)
        std::this_thread::yield();

    const std::shared_ptr<ControlledSource> source = s.opened.source;
    s.opened.release.reset();
    m.streamer.Update();
    for (const TerrainPayloadWant &want : wants)
        CHECK(m.streamer.Status(want.key).state == State::Absent);
    CHECK(m.streamer.Find(s.info.asset, s.info.release) == nullptr);
    CHECK(m.streamer.Stats().residentBytes == 0);
    CHECK(m.streamer.Stats().loadingBytes == 0);
    m.executor->RunAll();
    source->Resume();
    worker.join();
    m.streamer.Update();
    CHECK(m.streamer.Stats().discarded == 1);
    CHECK(source->RangeRequests().size() == 2);
    CHECK(m.streamer.Stats().ready == 0);
}

TEST_CASE("A want for a payload the index does not have fails without reading", "[terrain][streaming]")
{
    Streamed s;
    Manual m(StreamingSettings(uint64_t(1) << 24, Mode::Interest));
    m.streamer.Add(s.opened.release);
    TerrainPayloadKey missing = s.Tile({0, 0});
    missing.kind = TerrainPayloadKind::LodHeight;
    missing.level = 2;
    TerrainPayloadKey foreign = s.Tile({0, 0});
    foreign.release = TerrainReleaseId(0x404);
    const TerrainInterestId view = m.streamer.AddInterest();
    Want(m.streamer, view, {{missing, Demand::Required}, {foreign, Demand::Required}});
    m.Settle();
    CHECK(m.streamer.Status(missing).state == State::Failed);
    CHECK(m.streamer.Status(missing).error.code == Code::NotFound);
    CHECK(Mentions(m.streamer.Status(missing).error, "LodHeight tile (0, 0) layer 0 level 2"));
    CHECK(m.streamer.Status(foreign).state == State::Absent);
    CHECK(s.opened.source->RangeRequests().empty());
    m.clock.Advance(5.0);
    m.Settle();
    CHECK(s.opened.source->RangeRequests().empty());
}

TEST_CASE("Concurrent range reads of one file source return every payload intact", "[terrain][streaming]")
{
    const std::filesystem::path root = MakeScratchDir("streaming_disk");
    Streamed s(0xD15C);
    WriteRelease(root, s.info, s.encoded);
    const TerrainResult<TerrainReleaseHandle> opened = OpenTerrainRelease(root, s.info.asset, s.info.release);
    REQUIRE(opened);
    const auto handle = std::make_shared<const TerrainReleaseHandle>(*opened.value);

    std::vector<const TerrainTileRecord *> records;
    for (const auto *list : {&handle->Index().tiles, &handle->Index().regions})
        for (const TerrainTileRecord &record : *list)
            records.push_back(&record);
    std::atomic<size_t> intact{0};
    std::atomic<size_t> done{0};
    std::set<std::thread::id> threads;
    std::mutex mutex;
    const size_t rounds = 40;
    {
        TerrainWorkerPool pool(4);
        CHECK(pool.ThreadCount() == 4);
        for (size_t round = 0; round < rounds; ++round)
            for (const TerrainTileRecord *record : records)
                pool.Post([&, record]
                          {
                              const auto bytes = handle->ReadPayload(*record);
                              if (bytes && Fnv1a64(*bytes.value) == record->checksum)
                                  ++intact;
                              {
                                  std::lock_guard lock(mutex);
                                  threads.insert(std::this_thread::get_id());
                              }
                              ++done;
                          });
        while (done < rounds * records.size())
            std::this_thread::yield();
    }
    CHECK(intact == rounds * records.size());
    CHECK_FALSE(threads.contains(std::this_thread::get_id()));
    CHECK(threads.size() <= 4);

    auto release = TerrainResidentRelease::Create(*opened.value);
    TerrainStreamer streamer(StreamingSettings(uint64_t(1) << 30, Mode::Eager), std::make_shared<TerrainWorkerPool>(4));
    streamer.Add(release);
    REQUIRE(streamer.WaitForEager(s.info).code == Code::None);
    for (const TileHeights &tile : s.tiles)
    {
        const auto heights = streamer.Heights(s.Tile(tile.Tile()));
        REQUIRE(heights);
        CHECK(std::ranges::equal(heights->Samples(), tile.Samples()));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("The manual executor runs jobs in order only when asked", "[terrain][streaming]")
{
    TerrainManualExecutor executor;
    std::vector<int> ran;
    executor.Post([&] { ran.push_back(1); executor.Post([&] { ran.push_back(3); }); });
    executor.Post([&] { ran.push_back(2); });
    CHECK(executor.Pending() == 2);
    CHECK(ran.empty());
    CHECK(executor.RunOne());
    CHECK(ran == std::vector<int>{1});
    CHECK(executor.RunAll() == 2);
    CHECK(ran == std::vector<int>{1, 2, 3});
    CHECK_FALSE(executor.RunOne());
}

TEST_CASE("Moving through a map keeps residency within the budget and reads only on workers", "[terrain][streaming]")
{
    const auto layout = TerrainLayout::CreateFromTileSize(64.0, 4.0, {0.0, 0.0, 768.0, 768.0});
    REQUIRE(layout);
    const std::vector<TileHeights> tiles = BuildTiles(*layout);
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/moving.terrain.json"), 0x30BE);
    const TerrainEncodedRelease encoded =
        EncodeWithRegions(info, *layout, tiles, BuildTestRegions(*layout, tiles, 4, 8));
    ControlledRelease opened = OpenControlled(encoded);
    const TerrainIndex &index = opened.release->Handle().Index();
    const uint64_t each = TerrainPayloadFootprint(*index.Find({0, 0}));
    uint64_t regionMax = 0;
    for (const TerrainTileRecord &record : index.regions)
        regionMax = std::max(regionMax, TerrainPayloadFootprint(record));
    const uint64_t budget = 30 * each + 2 * regionMax;
    TerrainStreamer streamer(StreamingSettings(budget, Mode::Interest), std::make_shared<TerrainWorkerPool>(2));
    REQUIRE(streamer.Add(opened.release) == Mode::Interest);
    const TerrainInterestId view = streamer.AddInterest();
    const TileRange map = layout->Tiles();
    const auto lookup = [&](TileCoord owner)
    {
        for (const TileHeights &tile : tiles)
            if (tile.Tile() == owner)
                return tile.Samples();
        return std::span<const float>{};
    };

    const auto wantsAround = [&](glm::dvec2 at)
    {
        const TileCoord center{int32_t(at.x / 64.0), int32_t(at.y / 64.0)};
        std::vector<TerrainPayloadWant> wants;
        for (int32_t dz = -2; dz <= 2; ++dz)
            for (int32_t dx = -2; dx <= 2; ++dx)
            {
                const TileCoord tile{center.x + dx, center.z + dz};
                if (tile.x < map.min.x || tile.x > map.max.x || tile.z < map.min.z || tile.z > map.max.z)
                    continue;
                const int32_t ring = std::max(std::abs(dx), std::abs(dz));
                const Demand demand = ring == 0 ? Demand::Required : ring == 1 ? Demand::Visible : Demand::Prefetch;
                wants.push_back({TerrainPayloadKeyOf(info, *index.Find(tile)), demand, float(dx * dx + dz * dz)});
            }
        const TerrainRegionCoord region = index.regionGrid->Containing(0, center);
        wants.push_back({TerrainPayloadKeyOf(info, *index.FindRegion(region)), Demand::Visible, 0.0f});
        return wants;
    };

    const auto nearReady = [&](const std::vector<TerrainPayloadWant> &wants)
    {
        return std::all_of(wants.begin(), wants.end(),
                           [&](const TerrainPayloadWant &want)
                           { return want.demand == Demand::Prefetch || streamer.Status(want.key).state == State::Ready; });
    };

    size_t heights = 0;
    const int frames = 150;
    glm::dvec2 at{};
    for (int frame = 0; frame <= frames; ++frame)
    {
        at = glm::dvec2(16.0) + glm::dvec2(730.0, 690.0) * (double(frame) / frames);
        const std::vector<TerrainPayloadWant> wants = wantsAround(at);
        streamer.SetInterest(view, wants);
        streamer.Update();
        CheckWithinBudget(streamer);
        if (frame % 10 == 0)
        {
            UpdateUntil(streamer, [&] { return nearReady(wants); });
            CheckWithinBudget(streamer);
        }
        const auto local = TerrainLocalPosition::FromMeters({at.x, 0.0, at.y});
        REQUIRE(local);
        const TerrainHeightResult r = opened.release->Query().HeightAt(*local, {info, 4.0});
        REQUIRE((r.status == Status::Loading || r.status == Status::MeetsRequirement));
        if (r.HasHeight())
        {
            ++heights;
            CHECK(r.localHeight == EvaluateSurface(*layout, *local, lookup).height);
        }
        else
            CHECK(std::isnan(r.localHeight));
        streamer.WaitForCompletion(std::chrono::milliseconds(1));
    }

    const std::vector<TerrainPayloadWant> last = wantsAround(at);
    UpdateUntil(streamer, [&] { return nearReady(last); });
    CheckWithinBudget(streamer);
    const TerrainStreamingStats stats = streamer.Stats();
    CHECK(stats.peakBytes <= budget);
    CHECK(stats.evicted > 0);
    CHECK(stats.readsFailed == 0);
    CHECK(stats.maxUpdateSeconds > 0.0);
    CHECK(heights > 0);
    CHECK_FALSE(opened.source->ReaderThreads().contains(std::this_thread::get_id()));
}

TEST_CASE("Measure a mover crossing a map read from disk", "[.][streaming-measure]")
{
    const auto layout = TerrainLayout::CreateFromTileSize(256.0, 4.0, {0.0, 0.0, 10240.0, 10240.0});
    REQUIRE(layout);
    const std::vector<TileHeights> tiles = BuildTiles(*layout);
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/measure.terrain.json"), 0x3EA5);
    const TerrainEncodedRelease encoded =
        EncodeWithRegions(info, *layout, tiles, BuildTestRegions(*layout, tiles, 16, 128));
    const std::filesystem::path root = MakeScratchDir("streaming_measure");
    WriteRelease(root, info, encoded);
    const TerrainResult<TerrainReleaseHandle> opened = OpenTerrainRelease(root, info.asset, info.release);
    REQUIRE(opened);
    const std::shared_ptr<TerrainResidentRelease> release = TerrainResidentRelease::Create(*opened.value);
    const TerrainIndex &index = release->Handle().Index();

    const uint64_t budget = uint64_t(16) << 20;
    TerrainStreamer streamer(StreamingSettings(budget, Mode::Interest), std::make_shared<TerrainWorkerPool>(2));
    REQUIRE(streamer.Add(release) == Mode::Interest);
    const TerrainInterestId view = streamer.AddInterest();

    const double frame = 1.0 / 60.0;
    const double speed = 200.0;
    const glm::dvec2 from{500.0, 5120.0};
    const double distance = 9200.0;
    using Clock = std::chrono::steady_clock;
    std::vector<double> updateMs;
    std::vector<double> queryUs;
    std::vector<double> requiredMs;
    std::vector<double> visibleMs;
    std::unordered_map<TerrainPayloadKey, std::pair<double, Demand>> pending;
    size_t answered = 0;
    size_t frames = 0;
    const auto start = Clock::now();
    for (double t = 0.0; t * speed <= distance; t += frame, ++frames)
    {
        const glm::dvec2 at = from + glm::dvec2(t * speed, 0.0);
        std::vector<TerrainPayloadWant> wants;
        const TileCoord center{int32_t(at.x / 256.0), int32_t(at.y / 256.0)};
        for (const TerrainTileRecord &record : index.tiles)
        {
            const glm::dvec2 middle{(record.tile.x + 0.5) * 256.0, (record.tile.z + 0.5) * 256.0};
            const double d = glm::length(middle - at);
            const bool near = std::abs(record.tile.x - center.x) <= 1 && std::abs(record.tile.z - center.z) <= 1;
            if (near)
                wants.push_back({TerrainPayloadKeyOf(info, record), Demand::Required, float(d)});
            else if (d <= 1536.0)
                wants.push_back({TerrainPayloadKeyOf(info, record), Demand::Visible, float(d)});
            else if (d <= 2560.0)
                wants.push_back({TerrainPayloadKeyOf(info, record), Demand::Prefetch, float(d)});
        }
        for (const TerrainTileRecord &record : index.regions)
        {
            const TerrainLocalBounds rect = index.regionGrid->Rect({record.level, record.tile.x, record.tile.z});
            const glm::dvec2 middle{(rect.minX + rect.maxX) / 2.0, (rect.minZ + rect.maxZ) / 2.0};
            const double d = glm::length(middle - at);
            if (record.level == 0 && d <= 6000.0)
                wants.push_back({TerrainPayloadKeyOf(info, record), Demand::Visible, float(d)});
            else if (record.level == 1)
                wants.push_back({TerrainPayloadKeyOf(info, record), Demand::Prefetch, float(d)});
        }
        streamer.SetInterest(view, wants);

        const auto beforeUpdate = Clock::now();
        streamer.Update();
        const auto afterUpdate = Clock::now();
        updateMs.push_back(std::chrono::duration<double, std::milli>(afterUpdate - beforeUpdate).count());
        const auto local = TerrainLocalPosition::FromMeters({at.x, 0.0, at.y});
        const TerrainHeightResult r = release->Query().HeightAt(*local, {info, 4.0});
        queryUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - afterUpdate).count());
        answered += r.HasHeight() ? 1 : 0;
        CHECK((r.status == Status::Loading || r.status == Status::MeetsRequirement));
        CHECK(streamer.Stats().residentBytes + streamer.Stats().loadingBytes <= budget);

        const double now = std::chrono::duration<double>(Clock::now() - start).count();
        std::unordered_map<TerrainPayloadKey, std::pair<double, Demand>> still;
        for (const TerrainPayloadWant &want : wants)
        {
            if (want.demand == Demand::Prefetch)
                continue;
            const auto it = pending.find(want.key);
            const bool ready = streamer.Status(want.key).state == State::Ready;
            if (it == pending.end())
            {
                if (!ready)
                    still[want.key] = {now, want.demand};
                continue;
            }
            if (ready)
                (it->second.second == Demand::Required ? requiredMs : visibleMs)
                    .push_back((now - it->second.first) * 1000.0);
            else
                still[want.key] = it->second;
        }
        pending = std::move(still);
        std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>((frames + 1) * frame)));
    }

    const auto percentile = [](std::vector<double> values, double p)
    {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, size_t(p * double(values.size())))];
    };
    const TerrainStreamingStats stats = streamer.Stats();
    std::printf("frames %zu, wall %.1f s, answered %.1f%%\n", frames,
                std::chrono::duration<double>(Clock::now() - start).count(), 100.0 * double(answered) / double(frames));
    std::printf("Update ms P50 %.4f P95 %.4f P99 %.4f max %.4f\n", percentile(updateMs, 0.5), percentile(updateMs, 0.95),
                percentile(updateMs, 0.99), percentile(updateMs, 1.0));
    std::printf("HeightAt us P50 %.2f P99 %.2f max %.2f\n", percentile(queryUs, 0.5), percentile(queryUs, 0.99),
                percentile(queryUs, 1.0));
    std::printf("Required ready ms (n %zu) P50 %.1f P95 %.1f P99 %.1f max %.1f\n", requiredMs.size(),
                percentile(requiredMs, 0.5), percentile(requiredMs, 0.95), percentile(requiredMs, 0.99),
                percentile(requiredMs, 1.0));
    std::printf("Visible ready ms (n %zu) P50 %.1f P95 %.1f P99 %.1f max %.1f\n", visibleMs.size(),
                percentile(visibleMs, 0.5), percentile(visibleMs, 0.95), percentile(visibleMs, 0.99),
                percentile(visibleMs, 1.0));
    std::printf("peak %.2f MiB of %.2f MiB, resident %.2f MiB, reads %llu, read %.1f MiB, evicted %llu, canceled %llu, "
                "worker read %.3f s, decode %.3f s\n",
                stats.peakBytes / 1048576.0, budget / 1048576.0, stats.residentBytes / 1048576.0,
                (unsigned long long)stats.readsSucceeded, stats.bytesRead / 1048576.0,
                (unsigned long long)stats.evicted, (unsigned long long)stats.canceled, stats.readSeconds,
                stats.decodeSeconds);
    CHECK(stats.peakBytes <= budget);
    std::filesystem::remove_all(root);
}
