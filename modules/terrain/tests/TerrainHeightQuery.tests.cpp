/**
 * @file TerrainHeightQuery.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainHeightQuery.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;
    using Status = TerrainHeightStatus;
    using Failure = TerrainHeightFailure;
    constexpr double kInf = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    // About 5 m of twist in a 4 m cell, so the diagonal matters.
    double Hills(double x, double z)
    {
        return 180.0 + 40.0 * std::sin(0.011 * x + 0.7) * std::cos(0.017 * z - 0.4) +
               6.0 * std::sin(0.09 * x + 0.13 * z) + 3.0 * std::sin(0.37 * x) * std::sin(0.29 * z);
    }

    TerrainLocalPosition Local(double x, double z)
    {
        return *TerrainLocalPosition::FromMeters({x, 0.0, z});
    }

    WorldPosition World(double x, double y, double z)
    {
        return *WorldPosition::FromDoubleMeters({x, y, z});
    }

    // 2 x 2 tiles of 256 m with a four-tile corner at the origin.
    TerrainLayout OriginBlock(double spacing = 4.0)
    {
        const auto layout = TerrainLayout::CreateFromTileSize(256.0, spacing, {-256.0, -256.0, 256.0, 256.0});
        REQUIRE(layout);
        return *layout;
    }

    std::vector<TileHeights> HillTiles(const TerrainLayout &layout, double lift = 0.0)
    {
        std::vector<TileHeights> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, n);
                REQUIRE(tile);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        tile->Set(i, k,
                                  static_cast<float>(Hills(layout.SamplePosition(layout.TileFirstSample(x) + i),
                                                           layout.SamplePosition(layout.TileFirstSample(z) + k)) +
                                                     lift));
                tiles.push_back(std::move(*tile));
            }
        return tiles;
    }

    TileHeights Decimate(const TileHeights &fine)
    {
        std::optional<TileHeights> coarse = TileHeights::Create(fine.Tile(), fine.IntervalsPerTile() / 2);
        REQUIRE(coarse);
        for (int32_t k = 0; k <= coarse->IntervalsPerTile(); ++k)
            for (int32_t i = 0; i <= coarse->IntervalsPerTile(); ++i)
                coarse->Set(i, k, fine.At(2 * i, 2 * k));
        return *coarse;
    }

    std::vector<TileHeights> Decimate(const std::vector<TileHeights> &fine)
    {
        std::vector<TileHeights> coarse;
        for (const TileHeights &tile : fine)
            coarse.push_back(Decimate(tile));
        return coarse;
    }

    const TileHeights &TileAt(const std::vector<TileHeights> &tiles, TileCoord coord)
    {
        const auto it = std::find_if(tiles.begin(), tiles.end(), [coord](const TileHeights &t) { return t.Tile() == coord; });
        REQUIRE(it != tiles.end());
        return *it;
    }

    std::shared_ptr<const TileHeights> Shared(const std::vector<TileHeights> &tiles, TileCoord coord)
    {
        return std::make_shared<const TileHeights>(TileAt(tiles, coord));
    }

    TileSampleLookup LookupOf(const std::vector<TileHeights> &tiles)
    {
        return [&tiles](TileCoord coord) {
            for (const TileHeights &t : tiles)
                if (t.Tile() == coord)
                    return t.Samples();
            return std::span<const float>{};
        };
    }

    double MaxAbs(const TileHeights &tile)
    {
        double result = 0.0;
        for (const float h : tile.Samples())
            result = std::max(result, std::abs(double(h)));
        return result;
    }

    double MaxAbs(const std::vector<TileHeights> &tiles)
    {
        double result = 0.0;
        for (const TileHeights &tile : tiles)
            result = std::max(result, MaxAbs(tile));
        return result;
    }

    TerrainHeightRequest Need(const TerrainReleaseInfo &release, double spacing)
    {
        return {release, spacing};
    }

    bool NoHeight(const TerrainHeightResult &r)
    {
        return !r.HasHeight() && std::isnan(r.localHeight) && !r.worldPoint && std::isnan(r.spacing) && !r.maxError;
    }

    // Off-sample grid, both sides of every tile edge, four-tile corners and the bounds.
    std::vector<glm::dvec2> Probes(const TerrainLayout &layout)
    {
        const TerrainLocalBounds &b = layout.Bounds();
        const TileRange tiles = layout.Tiles();
        std::vector<glm::dvec2> points;
        for (double z = b.minZ; z <= b.maxZ; z += 3.3)
            for (double x = b.minX; x <= b.maxX; x += 3.3)
                points.push_back({x, z});

        const auto edge = [&layout](int32_t tile) { return layout.SamplePosition(layout.TileFirstSample(tile)); };
        for (int32_t t = tiles.min.x + 1; t <= tiles.max.x; ++t)
            for (double z = b.minZ; z <= b.maxZ; z += 0.25)
                for (const double x : {std::nextafter(edge(t), -kInf), edge(t), std::nextafter(edge(t), kInf)})
                    points.push_back({x, z});
        for (int32_t t = tiles.min.z + 1; t <= tiles.max.z; ++t)
            for (double x = b.minX; x <= b.maxX; x += 0.25)
                for (const double z : {std::nextafter(edge(t), -kInf), edge(t), std::nextafter(edge(t), kInf)})
                    points.push_back({x, z});

        for (int32_t tz = tiles.min.z + 1; tz <= tiles.max.z; ++tz)
            for (int32_t tx = tiles.min.x + 1; tx <= tiles.max.x; ++tx)
                for (const double dz : {-kInf, 0.0, kInf})
                    for (const double dx : {-kInf, 0.0, kInf})
                        points.push_back({dx == 0.0 ? edge(tx) : std::nextafter(edge(tx), dx),
                                          dz == 0.0 ? edge(tz) : std::nextafter(edge(tz), dz)});

        constexpr int kSteps = 512;
        for (int s = 0; s <= kSteps; ++s)
        {
            const double x = s == kSteps ? b.maxX : b.minX + (b.maxX - b.minX) * s / kSteps;
            const double z = s == kSteps ? b.maxZ : b.minZ + (b.maxZ - b.minZ) * s / kSteps;
            points.push_back({x, b.minZ});
            points.push_back({x, b.maxZ});
            points.push_back({b.minX, z});
            points.push_back({b.maxX, z});
        }
        return points;
    }

    struct Comparison
    {
        int probes = 0;
        int failures = 0;
        double worst = 0.0;
    };

    Comparison CompareWithSurface(TerrainHeightQuery &query, const TerrainLayout &layout,
                                  const std::vector<TileHeights> &tiles)
    {
        const TileSampleLookup lookup = LookupOf(tiles);
        const TerrainReleaseInfo &info = query.Release();
        Comparison c;
        for (const glm::dvec2 p : Probes(layout))
        {
            ++c.probes;
            const SurfaceSample expected = EvaluateSurface(layout, Local(p.x, p.y), lookup);
            const TerrainHeightResult r = query.HeightAt(Local(p.x, p.y), Need(info, 4.0));
            const bool ok = expected.IsOk() && r.status == Status::MeetsRequirement && r.tile == expected.tile &&
                            r.layer == 0 && r.spacing == 4.0 && r.sourceSpacing == 4.0 && r.release == info &&
                            r.maxError == SurfaceTolerance(MaxAbs(TileAt(tiles, r.tile))) && !r.worldPoint;
            if (!ok)
            {
                ++c.failures;
                continue;
            }
            c.worst = std::max(c.worst, std::abs(r.localHeight - expected.height));
        }
        return c;
    }

    // Deliberately not the evaluator's formula.
    double PlaneHeight(const std::array<glm::dvec3, 3> &v, double x, double z)
    {
        const double ax = v[1].x - v[0].x, az = v[1].z - v[0].z;
        const double bx = v[2].x - v[0].x, bz = v[2].z - v[0].z;
        const double px = x - v[0].x, pz = z - v[0].z;
        const double d = ax * bz - bx * az;
        const double w1 = (px * bz - bx * pz) / d;
        const double w2 = (ax * pz - px * az) / d;
        return (1.0 - w1 - w2) * v[0].y + w1 * v[1].y + w2 * v[2].y;
    }

    struct Preloaded
    {
        TerrainReleaseInfo info;
        TerrainLayout layout;
        std::vector<TileHeights> tiles;
        OpenedRelease opened;
        TerrainHeightQuery query;
    };

    Preloaded PreloadInMemory(const TerrainLayout &layout, uint64_t release, double lift = 0.0)
    {
        const TerrainReleaseInfo info = Info(TerrainAsset("terrain/query.terrain.json"), release);
        std::vector<TileHeights> tiles = HillTiles(layout, lift);
        OpenedRelease opened = OpenInMemory(Encode(info, layout, tiles));
        TerrainHeightQuery query = TerrainHeightQuery::Create(opened.handle);
        const TerrainPreloadReport report = PreloadAllTerrainTiles(query, opened.handle);
        INFO(report.firstError.message);
        REQUIRE(report.Ok());
        REQUIRE(report.loaded == tiles.size());
        return {info, layout, std::move(tiles), std::move(opened), std::move(query)};
    }

    struct TwoLayers
    {
        TerrainReleaseInfo info = Info(TerrainAsset("terrain/layers.terrain.json"), 0x117);
        TerrainLayout fineLayout = OriginBlock(4.0);
        TerrainLayout coarseLayout = OriginBlock(8.0);
        std::vector<TileHeights> fine = HillTiles(fineLayout);
        std::vector<TileHeights> coarse = Decimate(fine);
    };
}

TEST_CASE("A preloaded in-memory release answers like the canonical surface without reading", "[terrain][query]")
{
    for (const TerrainLayout &layout : {OriginBlock(), EdgeLayout()})
    {
        INFO("bounds " << layout.Bounds().minX << ", " << layout.Bounds().minZ << " to " << layout.Bounds().maxX
                       << ", " << layout.Bounds().maxZ);
        Preloaded p = PreloadInMemory(layout, 0x117A);
        const TerrainByteStats before = p.opened.source->Stats();
        p.opened.source->ClearRequests();

        const Comparison c = CompareWithSurface(p.query, layout, p.tiles);
        CHECK(c.probes > 20000);
        CHECK(c.failures == 0);
        CHECK(c.worst <= SurfaceTolerance(MaxAbs(p.tiles)));

        std::mt19937_64 random(0x117);
        const auto unit = [&random] { return double(random() >> 11) * 0x1.0p-53; };
        const TerrainLocalBounds &b = layout.Bounds();
        const TileSampleLookup lookup = LookupOf(p.tiles);
        double worstPlane = 0.0;
        for (int n = 0; n < 5000; ++n)
        {
            const double x = b.minX + unit() * (b.maxX - b.minX);
            const double z = b.minZ + unit() * (b.maxZ - b.minZ);
            const TerrainHeightResult r = p.query.HeightAt(Local(x, z), Need(p.info, 4.0));
            const SurfaceSample s = EvaluateSurface(layout, Local(x, z), lookup);
            REQUIRE(r.HasHeight());
            REQUIRE(s.IsOk());
            std::array<glm::dvec3, 3> triangle{};
            const std::array<GlobalSampleIndex, 3> &corners = TriangulateCell(s.cell).triangles[s.triangle];
            const TileHeights &tile = TileAt(p.tiles, s.tile);
            for (size_t v = 0; v < 3; ++v)
                triangle[v] = {layout.SamplePosition(corners[v].x),
                               tile.At(int32_t(corners[v].x - layout.TileFirstSample(s.tile.x)),
                                       int32_t(corners[v].z - layout.TileFirstSample(s.tile.z))),
                               layout.SamplePosition(corners[v].z)};
            worstPlane = std::max(worstPlane, std::abs(PlaneHeight(triangle, x, z) - r.localHeight));
        }
        CHECK(worstPlane <= SurfaceTolerance(MaxAbs(p.tiles)));

        CHECK(p.opened.source->Stats().reads == before.reads);
        CHECK(p.opened.source->Stats().bytes == before.bytes);
        CHECK(p.opened.source->Requests().empty());
        CHECK(p.query.PayloadRequests().empty());
    }
}

TEST_CASE("A release preloaded from disk answers without touching the files again", "[terrain][query]")
{
    const std::filesystem::path root = MakeScratchDir("query");
    const TerrainLayout layout = EdgeLayout();
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/disk.terrain.json"), 0xD15C);
    const std::vector<TileHeights> tiles = HillTiles(layout);
    WriteRelease(root, info, Encode(info, layout, tiles, {.chunkTiles = 2}));

    TerrainReleaseHandle handle = OpenOk(OpenTerrainRelease(root, info.asset, info.release));
    TerrainHeightQuery query = TerrainHeightQuery::Create(handle);
    CHECK(query.Release() == info);
    CHECK(query.LayerCount() == 1);
    CHECK(handle.Source().Stats().reads == 1);

    const TerrainPreloadReport report = PreloadAllTerrainTiles(query, handle);
    REQUIRE(report.Ok());
    CHECK(report.loaded == 9);
    // The index, then one range per tile.
    CHECK(handle.Source().Stats().reads == 1 + 9);
    const TerrainByteStats before = handle.Source().Stats();
    CHECK(before.bytes > 9 * 65 * 65 * 4);

    const Comparison c = CompareWithSurface(query, layout, tiles);
    CHECK(c.failures == 0);
    CHECK(c.worst <= SurfaceTolerance(MaxAbs(tiles)));
    CHECK(handle.Source().Stats().reads == before.reads);
    CHECK(handle.Source().Stats().bytes == before.bytes);

    CHECK(*query.ResidentTile(0, {-1, 0}) == TileAt(tiles, {-1, 0}));
    CHECK(query.ResidentTile(0, {-1, 0}) == query.ResidentTile(0, {-1, 0}));
    std::filesystem::remove_all(root);

    // The committed fixture release.
    TerrainReleaseHandle fixture =
        OpenOk(OpenCurrentTerrainRelease(std::filesystem::path(TERRAIN_FIXTURE_DIR) / "releases",
                                         TerrainAsset("terrain/fixture.terrain.json")));
    TerrainHeightQuery fixtureQuery = TerrainHeightQuery::Create(fixture);
    REQUIRE(PreloadAllTerrainTiles(fixtureQuery, fixture).loaded == 8);
    const TerrainHeightResult corner = fixtureQuery.HeightAt(Local(-8.0, 0.0), Need(fixture.Info(), 2.0));
    CHECK(corner.status == Status::MeetsRequirement);
    CHECK(corner.tile == TileCoord{-1, 0});
    CHECK(corner.localHeight == double(AnalyticHeight(-4, 0)));
}

TEST_CASE("Each result state is distinct and only height states carry a height", "[terrain][query]")
{
    const TwoLayers t;
    auto created = TerrainHeightQuery::Create(t.info, {{t.fineLayout}, {t.coarseLayout}});
    REQUIRE(created);
    TerrainHeightQuery &query = *created;
    const TileCoord tile{0, -1};
    const TerrainLocalPosition point = Local(37.3, -101.9);
    const TerrainLocalPosition sameTile = Local(200.0, -3.0);
    const TerrainPayloadRequest fineRequest{t.info.release, {t.info.asset, tile}, 0, 4.0};
    const TerrainPayloadRequest coarseRequest{t.info.release, {t.info.asset, tile}, 1, 8.0};

    SECTION("Nothing resident: loading, and the payload is requested once")
    {
        const TerrainHeightResult r = query.HeightAt(point, Need(t.info, 4.0));
        CHECK(r.status == Status::Loading);
        CHECK(NoHeight(r));
        CHECK(r.tile == tile);
        CHECK(r.layer == 0);
        CHECK(r.sourceSpacing == 4.0);
        CHECK(r.release == t.info);
        REQUIRE(query.PayloadRequests().size() == 1);
        CHECK(query.PayloadRequests()[0] == fineRequest);
        CHECK(query.Residency(0, tile) == TerrainTileResidency::Requested);

        CHECK(query.HeightAt(point, Need(t.info, 4.0)).status == Status::Loading);
        CHECK(query.HeightAt(sameTile, Need(t.info, 4.0)).status == Status::Loading);
        // Finer than the release has: still loading its finest, no new request.
        const TerrainHeightResult tooFine = query.HeightAt(point, Need(t.info, 1.0));
        CHECK(tooFine.status == Status::Loading);
        CHECK(tooFine.sourceSpacing > 1.0);
        CHECK(query.PayloadRequests().size() == 1);

        CHECK(query.HeightAt(point, Need(t.info, 8.0)).status == Status::Loading);
        REQUIRE(query.PayloadRequests().size() == 2);
        CHECK(query.PayloadRequests()[1] == coarseRequest);
    }

    SECTION("A coarse layer answers while the fine one loads")
    {
        REQUIRE(query.Insert(t.info, 1, Shared(t.coarse, tile)).code == Code::None);
        const SurfaceSample coarse = EvaluateSurface(t.coarseLayout, point, LookupOf(t.coarse));
        const SurfaceSample fine = EvaluateSurface(t.fineLayout, point, LookupOf(t.fine));

        const TerrainHeightResult loading = query.HeightAt(point, Need(t.info, 4.0));
        CHECK(loading.status == Status::CoarserWhileLoading);
        CHECK(loading.localHeight == coarse.height);
        CHECK(loading.localHeight != fine.height);
        CHECK(loading.spacing == 8.0);
        CHECK(loading.sourceSpacing == 4.0);
        CHECK(loading.layer == 1);
        CHECK_FALSE(loading.maxError);
        CHECK(std::vector<TerrainPayloadRequest>(query.PayloadRequests().begin(), query.PayloadRequests().end()) ==
              std::vector<TerrainPayloadRequest>{fineRequest});

        const TerrainHeightResult coarseEnough = query.HeightAt(point, Need(t.info, 8.0));
        CHECK(coarseEnough.status == Status::MeetsRequirement);
        CHECK(coarseEnough.localHeight == coarse.height);
        CHECK_FALSE(coarseEnough.maxError);

        const TerrainHeightResult limited = query.HeightAt(point, Need(t.info, 2.0));
        CHECK(limited.status == Status::CoarserAtSourceLimit);
        CHECK(limited.spacing == 8.0);
        CHECK(limited.sourceSpacing == 4.0);
        CHECK(query.PayloadRequests().size() == 1);

        REQUIRE(query.Insert(t.info, 0, Shared(t.fine, tile)).code == Code::None);
        CHECK(query.PayloadRequests().empty());
        const double tolerance = SurfaceTolerance(MaxAbs(TileAt(t.fine, tile)));
        for (const double need : {4.0, 8.0, 100.0})
        {
            const TerrainHeightResult r = query.HeightAt(point, Need(t.info, need));
            CHECK(r.status == Status::MeetsRequirement);
            CHECK(r.localHeight == fine.height);
            CHECK(r.layer == 0);
            CHECK(r.maxError == tolerance);
        }

        const TerrainHeightResult never = query.HeightAt(point, Need(t.info, 2.0));
        CHECK(never.status == Status::CoarserAtSourceLimit);
        CHECK(never.localHeight == fine.height);
        CHECK(never.spacing == never.sourceSpacing);
        CHECK(never.maxError == tolerance);
        CHECK(query.PayloadRequests().empty());
    }

    SECTION("Outside the map and failures carry no height")
    {
        REQUIRE(query.Insert(t.info, 0, Shared(t.fine, tile)).code == Code::None);

        const TerrainHeightResult outside = query.HeightAt(Local(256.5, 0.0), Need(t.info, 4.0));
        CHECK(outside.status == Status::OutsideMap);
        CHECK(NoHeight(outside));
        CHECK(std::isnan(outside.sourceSpacing));
        CHECK(query.HeightAt(Local(std::nextafter(-256.0, -kInf), 0.0), Need(t.info, 4.0)).status ==
              Status::OutsideMap);
        CHECK(query.PayloadRequests().empty());

        TerrainReleaseInfo other = t.info;
        other.release = TerrainReleaseId(0x118);
        const TerrainHeightResult wrong = query.HeightAt(point, Need(other, 4.0));
        CHECK(wrong.status == Status::Failed);
        CHECK(wrong.failure == Failure::WrongRelease);
        CHECK(wrong.release == t.info);
        CHECK(NoHeight(wrong));

        for (const double need : {0.0, -4.0, kNaN, kInf})
        {
            const TerrainHeightResult invalid = query.HeightAt(point, Need(t.info, need));
            CHECK(invalid.failure == Failure::InvalidInput);
            CHECK(NoHeight(invalid));
        }

        // A coarse tile is resident, but the needed payload failed: no fallback.
        const TileCoord broken{-1, -1};
        REQUIRE(query.Insert(t.info, 1, Shared(t.coarse, broken)).code == Code::None);
        CHECK(query.HeightAt(Local(-10.0, -10.0), Need(t.info, 4.0)).status == Status::CoarserWhileLoading);
        REQUIRE(query.MarkFailed(t.info, 0, broken, {Code::Corrupt, "payload checksum"}).code == Code::None);
        CHECK(query.PayloadRequests().empty());
        const TerrainHeightResult failed = query.HeightAt(Local(-10.0, -10.0), Need(t.info, 4.0));
        CHECK(failed.status == Status::Failed);
        CHECK(failed.failure == Failure::LoadFailed);
        CHECK(failed.layer == 0);
        CHECK(failed.tile == broken);
        CHECK(NoHeight(failed));
        REQUIRE(query.TileError(0, broken));
        CHECK(query.TileError(0, broken)->code == Code::Corrupt);
        CHECK(query.HeightAt(Local(-10.0, -10.0), Need(t.info, 8.0)).status == Status::MeetsRequirement);
    }
}

TEST_CASE("A per-tile source limit never resolves by waiting, a loading fine tile does", "[terrain][query]")
{
    const TwoLayers t;
    // Only tile (0, 0) is baked at 4 m; the rest only at 8 m.
    auto created = TerrainHeightQuery::Create(
        t.info, {{t.fineLayout, std::vector<TileCoord>{{0, 0}}}, {t.coarseLayout}});
    REQUIRE(created);
    TerrainHeightQuery &query = *created;
    for (const TileHeights &tile : t.coarse)
        REQUIRE(query.Insert(t.info, 1, std::make_shared<const TileHeights>(tile)).code == Code::None);

    const TerrainLocalPosition coarseOnly = Local(-100.5, 30.25);
    const TerrainLocalPosition fineTile = Local(100.5, 30.25);
    // On the x = 0 edge, tile (0, 0) owns the point.
    const TerrainLocalPosition onEdge = Local(0.0, 30.25);

    const TerrainHeightResult limited = query.HeightAt(coarseOnly, Need(t.info, 4.0));
    CHECK(limited.status == Status::CoarserAtSourceLimit);
    CHECK(limited.spacing == 8.0);
    CHECK(limited.sourceSpacing == 8.0);
    CHECK(limited.maxError == SurfaceTolerance(MaxAbs(TileAt(t.coarse, {-1, 0}))));
    CHECK(query.PayloadRequests().empty());

    const TerrainHeightResult loading = query.HeightAt(fineTile, Need(t.info, 4.0));
    CHECK(loading.status == Status::CoarserWhileLoading);
    CHECK(loading.sourceSpacing == 4.0);
    CHECK_FALSE(loading.maxError);
    CHECK(query.HeightAt(onEdge, Need(t.info, 4.0)).status == Status::CoarserWhileLoading);
    REQUIRE(query.PayloadRequests().size() == 1);
    CHECK(query.PayloadRequests()[0].tile.coord == TileCoord{0, 0});

    REQUIRE(query.Insert(t.info, 0, Shared(t.fine, {0, 0})).code == Code::None);
    CHECK(query.HeightAt(fineTile, Need(t.info, 4.0)).status == Status::MeetsRequirement);
    CHECK(query.HeightAt(onEdge, Need(t.info, 4.0)).status == Status::MeetsRequirement);
    CHECK(query.HeightAt(coarseOnly, Need(t.info, 4.0)).status == Status::CoarserAtSourceLimit);
    CHECK(query.Insert(t.info, 0, Shared(t.fine, {-1, 0})).code == Code::NotFound);
}

TEST_CASE("A coarse layer's error is known only when the bake measured it", "[terrain][query]")
{
    const TwoLayers t;
    const TileSampleLookup fine = LookupOf(t.fine);
    const TileSampleLookup coarse = LookupOf(t.coarse);
    // Coarse cell edges lie on fine ones, so the deviation peaks on the 4 m samples.
    double deviation = 0.0;
    for (double z = -256.0; z <= 256.0; z += 4.0)
        for (double x = -256.0; x <= 256.0; x += 4.0)
            deviation = std::max(deviation, std::abs(EvaluateSurface(t.coarseLayout, Local(x, z), coarse).height -
                                                     EvaluateSurface(t.fineLayout, Local(x, z), fine).height));
    REQUIRE(deviation > 0.0);

    auto created = TerrainHeightQuery::Create(t.info, {{t.fineLayout}, {t.coarseLayout, std::nullopt, deviation}});
    REQUIRE(created);
    for (const TileHeights &tile : t.coarse)
        REQUIRE(created->Insert(t.info, 1, std::make_shared<const TileHeights>(tile)).code == Code::None);

    double worst = 0.0;
    for (double z = -255.75; z < 256.0; z += 3.5)
        for (double x = -255.75; x < 256.0; x += 3.5)
        {
            const TerrainHeightResult r = created->HeightAt(Local(x, z), Need(t.info, 4.0));
            REQUIRE(r.status == Status::CoarserWhileLoading);
            REQUIRE(r.maxError);
            CHECK(*r.maxError == deviation + SurfaceTolerance(MaxAbs(TileAt(t.coarse, r.tile))));
            worst = std::max(worst, std::abs(r.localHeight - EvaluateSurface(t.fineLayout, Local(x, z), fine).height) -
                                        *r.maxError);
        }
    CHECK(worst <= 0.0);
}

TEST_CASE("Payload requests hand a loader each missing tile once", "[terrain][query]")
{
    const TerrainLayout layout = OriginBlock();
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/requests.terrain.json"), 0x117C);
    const std::vector<TileHeights> tiles = HillTiles(layout);
    OpenedRelease opened = OpenInMemory(Encode(info, layout, tiles));
    TerrainHeightQuery query = TerrainHeightQuery::Create(opened.handle);
    const TerrainByteStats start = opened.source->Stats();

    const glm::dvec2 points[]{{-10.0, -10.0}, {-20.0, -30.0}, {10.0, -10.0}, {0.0, 0.0}, {-0.5, 255.0}};
    for (const glm::dvec2 p : points)
        CHECK(query.HeightAt(Local(p.x, p.y), Need(info, 4.0)).status == Status::Loading);
    CHECK(opened.source->Stats().reads == start.reads);
    std::vector<TerrainPayloadRequest> taken = query.TakePayloadRequests();
    REQUIRE(taken.size() == 4);
    CHECK(taken[0].tile == TileKey{info.asset, {-1, -1}});
    CHECK(taken[1].tile == TileKey{info.asset, {0, -1}});
    CHECK(taken[2].tile == TileKey{info.asset, {0, 0}});
    CHECK(taken[3].tile == TileKey{info.asset, {-1, 0}});
    for (const TerrainPayloadRequest &request : taken)
    {
        CHECK(request.release == info.release);
        CHECK(request.spacing == 4.0);
    }

    // Taken but not delivered: still pending, never asked twice.
    CHECK(query.PayloadRequests().empty());
    CHECK(query.HeightAt(Local(-10.0, -10.0), Need(info, 4.0)).status == Status::Loading);
    CHECK(query.PayloadRequests().empty());
    CHECK(query.Residency(0, {-1, -1}) == TerrainTileResidency::Requested);

    for (const TerrainPayloadRequest &request : taken)
    {
        TerrainResult<TileHeights> read = opened.handle.ReadTile(request.tile);
        REQUIRE(read);
        REQUIRE(query.Insert(info, request.layer, std::make_shared<const TileHeights>(std::move(*read.value))).code ==
                Code::None);
    }
    CHECK(opened.source->Stats().reads == start.reads + 4);
    const TileSampleLookup lookup = LookupOf(tiles);
    for (const glm::dvec2 p : points)
    {
        const TerrainHeightResult r = query.HeightAt(Local(p.x, p.y), Need(info, 4.0));
        CHECK(r.status == Status::MeetsRequirement);
        CHECK(r.localHeight == EvaluateSurface(layout, Local(p.x, p.y), lookup).height);
    }
    CHECK(opened.source->Stats().reads == start.reads + 4);
    CHECK(query.PayloadRequests().empty());
}

TEST_CASE("An evicted tile is requested again and answered tiles are reported once per take", "[terrain][query]")
{
    const TerrainLayout layout = OriginBlock();
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/evict.terrain.json"), 0x126E);
    OpenedRelease opened = OpenInMemory(Encode(info, layout, HillTiles(layout)));
    TerrainHeightQuery query = TerrainHeightQuery::Create(opened.handle);
    REQUIRE(PreloadAllTerrainTiles(query, opened.handle).Ok());
    CHECK(query.TakeUsedTiles().empty());

    for (int i = 0; i < 3; ++i)
    {
        CHECK(query.HeightAt(Local(-10.0, -10.0), Need(info, 4.0)).status == Status::MeetsRequirement);
        CHECK(query.HeightAt(Local(10.0, 10.0), Need(info, 4.0)).status == Status::MeetsRequirement);
    }
    const std::vector<TerrainPayloadRequest> used = query.TakeUsedTiles();
    REQUIRE(used.size() == 2);
    CHECK(used[0].tile == TileKey{info.asset, {-1, -1}});
    CHECK(used[1].tile == TileKey{info.asset, {0, 0}});
    CHECK(used[0].release == info.release);
    CHECK(used[0].layer == 0);
    CHECK(query.TakeUsedTiles().empty());
    CHECK(query.HeightAt(Local(10.0, 10.0), Need(info, 4.0)).status == Status::MeetsRequirement);
    CHECK(query.TakeUsedTiles().size() == 1);

    query.Evict(0, {0, 0});
    CHECK(query.Residency(0, {0, 0}) == TerrainTileResidency::Absent);
    CHECK(query.ResidentTile(0, {0, 0}) == nullptr);
    const TerrainHeightResult r = query.HeightAt(Local(10.0, 10.0), Need(info, 4.0));
    CHECK(r.status == Status::Loading);
    CHECK(std::isnan(r.localHeight));
    CHECK(query.TakeUsedTiles().empty());
    REQUIRE(query.PayloadRequests().size() == 1);
    CHECK(query.PayloadRequests()[0].tile == TileKey{info.asset, {0, 0}});

    query.Evict(0, {0, 0});
    CHECK(query.PayloadRequests().empty());
    REQUIRE(query.MarkFailed(info, 0, {-1, -1}, {Code::Io, "gone"}).code == Code::None);
    query.Evict(0, {-1, -1});
    CHECK(query.HeightAt(Local(-10.0, -10.0), Need(info, 4.0)).status == Status::Loading);
    query.Evict(7, {0, 0});
}

TEST_CASE("Corrupt or missing payloads fail their tiles and never read as height zero", "[terrain][query]")
{
    const TerrainLayout layout = OriginBlock();
    const TerrainReleaseInfo info = Info(TerrainAsset("terrain/broken.terrain.json"), 0xBAD);
    const std::vector<TileHeights> tiles = HillTiles(layout);
    const TerrainEncodedRelease encoded = Encode(info, layout, tiles);
    OpenedRelease opened = OpenInMemory(encoded);
    const TerrainIndex &index = opened.handle.Index();

    const TerrainTileRecord *corrupt = index.Find({0, 0});
    REQUIRE(corrupt);
    const std::string corruptPack = TerrainPackFileName(index.packs[corrupt->pack].chunk);
    std::vector<std::byte> bytes = *opened.source->Get(corruptPack);
    bytes[static_cast<size_t>(corrupt->offset) + 5] ^= std::byte{0x40};
    opened.source->Put(corruptPack, std::move(bytes));
    opened.source->Remove(TerrainPackFileName(index.packs[index.Find({-1, 0})->pack].chunk));

    TerrainHeightQuery query = TerrainHeightQuery::Create(opened.handle);
    const TerrainPreloadReport report = PreloadAllTerrainTiles(query, opened.handle);
    CHECK(report.loaded == 2);
    CHECK(report.failed == 2);
    CHECK(report.firstError.code == Code::Io);
    CHECK(query.Residency(0, {0, 0}) == TerrainTileResidency::Failed);
    REQUIRE(query.TileError(0, {0, 0}));
    CHECK(query.TileError(0, {0, 0})->code == Code::Corrupt);
    REQUIRE(query.TileError(0, {-1, 0}));
    CHECK(query.TileError(0, {-1, 0})->code == Code::Io);
    CHECK_FALSE(query.ResidentTile(0, {0, 0}));

    const TerrainPlacement placement{info.asset, World(0.0, 0.0, 0.0)};
    for (const glm::dvec2 p : {glm::dvec2{100.0, 100.0}, glm::dvec2{-100.0, 100.0}, glm::dvec2{0.0, 0.0}})
    {
        INFO("probe " << p.x << ", " << p.y);
        for (const TerrainHeightResult &r : {query.HeightAt(Local(p.x, p.y), Need(info, 4.0)),
                                             query.HeightAt(World(p.x, 0.0, p.y), placement, Need(info, 4.0))})
        {
            CHECK(r.status == Status::Failed);
            CHECK(r.failure == Failure::LoadFailed);
            CHECK(NoHeight(r));
        }
    }
    CHECK(query.HeightAt(Local(-100.0, -100.0), Need(info, 4.0)).status == Status::MeetsRequirement);
    CHECK(query.HeightAt(Local(100.0, -100.0), Need(info, 4.0)).status == Status::MeetsRequirement);
    CHECK(query.PayloadRequests().empty());
}

TEST_CASE("A query bound to release A keeps answering A after B is published", "[terrain][query]")
{
    const std::filesystem::path root = MakeScratchDir("query_pointer");
    const TerrainLayout layout = OriginBlock();
    const AssetId asset = TerrainAsset("terrain/pinned.terrain.json");
    const TerrainReleaseInfo infoA = Info(asset, 0xA117);
    const TerrainReleaseInfo infoB = Info(asset, 0xB117);
    const std::vector<TileHeights> tilesA = HillTiles(layout);
    const std::vector<TileHeights> tilesB = HillTiles(layout, 10.0);

    WriteRelease(root, infoA, Encode(infoA, layout, tilesA));
    PointCurrentAt(root, asset, infoA.release);
    const TerrainReleaseHandle handleA = OpenOk(OpenCurrentTerrainRelease(root, asset));
    TerrainHeightQuery queryA = TerrainHeightQuery::Create(handleA);
    REQUIRE(PreloadAllTerrainTiles(queryA, handleA).Ok());

    // B is published and the pointer moves to it.
    WriteRelease(root, infoB, Encode(infoB, layout, tilesB));
    REQUIRE(WriteTextAtomic(TerrainCurrentPointerPath(root, asset), EncodeTerrainCurrentPointer(infoB.release)));
    const TerrainReleaseHandle handleB = OpenOk(OpenCurrentTerrainRelease(root, asset));
    REQUIRE(handleB.Info() == infoB);

    const TerrainByteStats before = handleA.Source().Stats();
    const TileSampleLookup lookupA = LookupOf(tilesA);
    const TileSampleLookup lookupB = LookupOf(tilesB);
    for (const glm::dvec2 p : {glm::dvec2{-50.25, 12.5}, glm::dvec2{0.0, 0.0}, glm::dvec2{255.0, -255.0}})
    {
        const TerrainHeightResult r = queryA.HeightAt(Local(p.x, p.y), Need(infoA, 4.0));
        CHECK(r.status == Status::MeetsRequirement);
        CHECK(r.release == infoA);
        CHECK(r.localHeight == EvaluateSurface(layout, Local(p.x, p.y), lookupA).height);
        CHECK(r.localHeight != EvaluateSurface(layout, Local(p.x, p.y), lookupB).height);

        const TerrainHeightResult wrong = queryA.HeightAt(Local(p.x, p.y), Need(infoB, 4.0));
        CHECK(wrong.failure == Failure::WrongRelease);
        CHECK(NoHeight(wrong));
    }
    CHECK(handleA.Source().Stats().reads == before.reads);

    const TerrainPreloadReport refused = PreloadAllTerrainTiles(queryA, handleB);
    CHECK(refused.firstError.code == Code::WrongIdentity);
    CHECK(refused.loaded == 0);
    CHECK(queryA.Insert(infoB, 0, Shared(tilesB, {0, 0})).code == Code::WrongIdentity);
    CHECK(queryA.MarkFailed(infoB, 0, {0, 0}, {Code::Io, "x"}).code == Code::WrongIdentity);
    CHECK(*queryA.ResidentTile(0, {0, 0}) == TileAt(tilesA, {0, 0}));

    TerrainHeightQuery queryB = TerrainHeightQuery::Create(handleB);
    REQUIRE(PreloadAllTerrainTiles(queryB, handleB).Ok());
    const TerrainHeightResult b = queryB.HeightAt(Local(-50.25, 12.5), Need(infoB, 4.0));
    CHECK(b.release == infoB);
    CHECK(b.localHeight == EvaluateSurface(layout, Local(-50.25, 12.5), lookupB).height);
    std::filesystem::remove_all(root);
}

TEST_CASE("World queries go through the placement and report the world height", "[terrain][query]")
{
    Preloaded p = PreloadInMemory(OriginBlock(), 0x117D);
    const TerrainPlacement placement{p.info.asset, World(12.5, -3.25, 40.0)};

    const TerrainHeightResult r = p.query.HeightAt(World(-25.75, 999.0, 80.5), placement, Need(p.info, 4.0));
    REQUIRE(r.status == Status::MeetsRequirement);
    const TerrainHeightResult local = p.query.HeightAt(Local(-38.25, 40.5), Need(p.info, 4.0));
    CHECK(r.localHeight == local.localHeight);
    REQUIRE(r.worldPoint);
    CHECK(r.worldPoint->Meters() ==
          WorldVec3(WorldReal(-25.75), static_cast<WorldReal>(-3.25 + local.localHeight), WorldReal(80.5)));

    const TerrainHeightResult outside = p.query.HeightAt(World(300.0, 0.0, 0.0), placement, Need(p.info, 4.0));
    CHECK(outside.status == Status::OutsideMap);
    CHECK(NoHeight(outside));

    const TerrainPlacement otherAsset{TerrainAsset("terrain/other.terrain.json"), placement.translation};
    const TerrainHeightResult wrong = p.query.HeightAt(World(-25.75, 0.0, 80.5), otherAsset, Need(p.info, 4.0));
    CHECK(wrong.failure == Failure::WrongRelease);
    CHECK(NoHeight(wrong));

    if constexpr (mir::kLargeWorld)
    {
        const TerrainPlacement far{p.info.asset, World(-1.0e308, 0.0, 0.0)};
        const TerrainHeightResult overflow = p.query.HeightAt(World(1.0e308, 0.0, 0.0), far, Need(p.info, 4.0));
        CHECK(overflow.failure == Failure::InvalidInput);
        CHECK(NoHeight(overflow));
    }

    // Fits a double world position but not a float one.
    auto tall = TerrainHeightQuery::Create(p.info, {{p.layout}});
    REQUIRE(tall);
    const int32_t side = p.layout.IntervalsPerTile() + 1;
    std::optional<TileHeights> block = TileHeights::Create({0, 0}, p.layout.IntervalsPerTile(),
                                                           std::vector<float>(size_t(side * side), 3.0e38f));
    REQUIRE(block);
    REQUIRE(tall->Insert(p.info, 0, std::make_shared<const TileHeights>(std::move(*block))).code == Code::None);
    const TerrainPlacement high{p.info.asset, World(0.0, 1.0e38, 0.0)};
    const TerrainHeightResult r2 = tall->HeightAt(World(10.0, 0.0, 10.0), high, Need(p.info, 4.0));
    if constexpr (mir::kLargeWorld)
    {
        CHECK(r2.status == Status::MeetsRequirement);
        REQUIRE(r2.worldPoint);
        CHECK(r2.worldPoint->Meters().y == 1.0e38 + double(3.0e38f));
    }
    else
    {
        CHECK(r2.failure == Failure::InvalidInput);
        CHECK(NoHeight(r2));
    }
}

TEST_CASE("World queries at far placements keep double precision", "[terrain][query][far]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    Preloaded p = PreloadInMemory(OriginBlock(), 0x117F);
    const double tolerance = SurfaceTolerance(MaxAbs(p.tiles));
    const glm::dvec3 placements[]{{-150000.125, 350.5, 149999.875}, {150000.25, -20.0, -150000.75}};
    const glm::dvec2 offsets[]{{-37.375, 101.625}, {0.0, -200.5}, {0.0, 0.0}, {123.001, -45.999}, {123.002, -45.999}};

    for (const glm::dvec3 &origin : placements)
    {
        INFO("placement " << origin.x << ", " << origin.y << ", " << origin.z);
        const TerrainPlacement placement{p.info.asset, World(origin.x, origin.y, origin.z)};
        double previous = kNaN;
        for (const glm::dvec2 offset : offsets)
        {
            const glm::dvec3 world(origin.x + offset.x, 0.0, origin.z + offset.y);
            const TerrainHeightResult r =
                p.query.HeightAt(World(world.x, world.y, world.z), placement, Need(p.info, 4.0));
            REQUIRE(r.status == Status::MeetsRequirement);

            const glm::dvec3 local = TryWorldToTerrainLocal(World(world.x, 0.0, world.z), placement.translation)->Meters();
            CHECK(std::abs(local.x - offset.x) < 1e-10);
            CHECK(std::abs(local.z - offset.y) < 1e-10);
            const TerrainHeightResult direct = p.query.HeightAt(Local(local.x, local.z), Need(p.info, 4.0));
            CHECK(r.localHeight == direct.localHeight);
            CHECK(std::abs(r.localHeight - EvaluateSurface(p.layout, Local(offset.x, offset.y), LookupOf(p.tiles)).height) <=
                  tolerance + 1e-9);

            REQUIRE(r.worldPoint);
            CHECK(r.worldPoint->Meters().y == origin.y + r.localHeight);
            CHECK(std::abs(r.worldPoint->Meters().x - world.x) <= 1e-10);
            CHECK(std::abs(r.worldPoint->Meters().z - world.z) <= 1e-10);

            // 1 mm along x on a slope still changes the height at 150 km.
            if (offset.x == 123.002)
                CHECK(r.localHeight != previous);
            previous = r.localHeight;
        }
    }

    // float holds 150 km only to 1/64 m.
    CHECK(static_cast<float>(-150000.125 + 123.001) == static_cast<float>(-150000.125 + 123.002));
}

TEST_CASE("Create refuses layers that do not nest and Insert refuses foreign or malformed tiles", "[terrain][query]")
{
    const TwoLayers t;
    const auto make = [&t](std::vector<TerrainHeightLayer> layers) {
        return TerrainHeightQuery::Create(t.info, std::move(layers)).has_value();
    };
    const auto layout = [](double spacing, int32_t n, TerrainLocalBounds bounds = {-256.0, -256.0, 256.0, 256.0}) {
        const auto created = TerrainLayout::Create(spacing, n, bounds);
        REQUIRE(created);
        return *created;
    };

    CHECK(make({{t.fineLayout}, {t.coarseLayout}}));
    CHECK(make({{t.fineLayout}, {t.coarseLayout}, {layout(16.0, 16)}}));
    CHECK_FALSE(make({}));
    CHECK_FALSE(TerrainHeightQuery::Create(Info(kNullAssetId, 1), {{t.fineLayout}}));
    CHECK_FALSE(TerrainHeightQuery::Create(Info(t.info.asset, 0), {{t.fineLayout}}));
    CHECK_FALSE(make({{t.coarseLayout}, {t.fineLayout}}));
    CHECK_FALSE(make({{t.fineLayout}, {t.fineLayout}}));
    // 8 m x 16 is a 128 m tile; 12 m is 3 x 4 m, but 3 does not divide 64.
    CHECK_FALSE(make({{t.fineLayout}, {layout(8.0, 16)}}));
    CHECK_FALSE(make({{t.fineLayout}, {layout(12.0, 21)}}));
    CHECK_FALSE(make({{t.fineLayout}, {layout(8.0, 32, {-256.0, -256.0, 256.0, 250.0})}}));
    CHECK_FALSE(make({{t.fineLayout}, {t.coarseLayout, std::nullopt, -1.0}}));
    CHECK_FALSE(make({{t.fineLayout}, {t.coarseLayout, std::nullopt, kNaN}}));
    CHECK_FALSE(make({{t.fineLayout, std::vector<TileCoord>{{5, 5}}}, {t.coarseLayout}}));
    CHECK_FALSE(make({{t.fineLayout, std::vector<TileCoord>{{0, 0}, {-1, 0}}},
                      {t.coarseLayout, std::vector<TileCoord>{{0, -1}}}}));

    auto created = TerrainHeightQuery::Create(t.info, {{t.fineLayout}, {t.coarseLayout}});
    REQUIRE(created);
    TerrainHeightQuery &query = *created;
    TerrainReleaseInfo other = t.info;
    other.source = TerrainSourceFingerprint(0xbeef);
    CHECK(query.Insert(other, 0, Shared(t.fine, {0, 0})).code == Code::WrongIdentity);
    CHECK(query.Insert(t.info, 2, Shared(t.fine, {0, 0})).code == Code::InvalidInput);
    CHECK(query.Insert(t.info, 0, nullptr).code == Code::InvalidInput);
    CHECK(query.Insert(t.info, 1, Shared(t.fine, {0, 0})).code == Code::InvalidInput);
    std::optional<TileHeights> outside = TileHeights::Create({2, 0}, 64);
    REQUIRE(outside);
    CHECK(query.Insert(t.info, 0, std::make_shared<const TileHeights>(*outside)).code == Code::NotFound);
    TileHeights holed = TileAt(t.fine, {0, 0});
    holed.Set(3, 7, std::numeric_limits<float>::quiet_NaN());
    CHECK(query.Insert(t.info, 0, std::make_shared<const TileHeights>(holed)).code == Code::InvalidInput);
    CHECK(query.Residency(0, {0, 0}) == TerrainTileResidency::Absent);
    CHECK(query.Residency(1, {0, 0}) == TerrainTileResidency::Absent);
    CHECK(query.Residency(7, {0, 0}) == TerrainTileResidency::Absent);

    const OpenedRelease opened = OpenInMemory(Encode(t.info, t.coarseLayout, t.coarse));
    auto fineOnly = TerrainHeightQuery::Create(t.info, {{t.fineLayout}});
    REQUIRE(fineOnly);
    CHECK(PreloadAllTerrainTiles(*fineOnly, opened.handle).firstError.code == Code::InvalidInput);
    CHECK(opened.source->Stats().reads == 1);
}
