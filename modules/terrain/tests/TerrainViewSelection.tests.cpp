/**
 * @file TerrainViewSelection.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainViewSelection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainLod.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainViewSelection.h>

#include <core/math/WorldReal.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Demand = TerrainDemand;
    using Kind = TerrainUnitKind;

    constexpr double kTileSize = 64.0;
    constexpr double kSpacing = 4.0;
    constexpr TerrainLocalBounds kBounds{-200.0, -136.0, 312.0, 376.0};

    float Relief(int64_t i, int64_t k)
    {
        const double x = double(i) * kSpacing, z = double(k) * kSpacing;
        return float(40.0 * std::sin(x / 180.0) * std::cos(z / 210.0) + 9.0 * std::sin((x + z) / 47.0) +
                     2.0 * std::cos(x / 11.0) * std::sin(z / 13.0));
    }

    std::vector<TileHeights> ReliefTiles(const TerrainLayout &layout)
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
                        tile->Set(i, k, Relief(layout.TileFirstSample(x) + i, layout.TileFirstSample(z) + k));
                tiles.push_back(std::move(*tile));
            }
        return tiles;
    }

    std::vector<TerrainPayloadWrite> BuildTestLods(std::span<const TerrainLayout> layers,
                                                   std::span<const TileHeights> tiles)
    {
        std::vector<TerrainPayloadWrite> writes;
        const TerrainLayout &source = layers[0];
        for (const TileHeights &tile : tiles)
            for (uint32_t level = 0; level < layers.size(); ++level)
            {
                const TerrainLayout &layout = layers[level];
                const int32_t n = layout.IntervalsPerTile(), stride = source.IntervalsPerTile() / n;
                TerrainLodSurface lod;
                lod.tile = tile.Tile();
                lod.layer = level;
                lod.level = level;
                lod.sourceSpacing = source.SampleSpacing();
                lod.spacing = layout.SampleSpacing();
                lod.sourceChecksum = TerrainFinalHeightChecksum(tile);
                lod.origin = *TerrainLocalPosition::FromMeters(
                    {source.SamplePosition(source.TileFirstSample(tile.Tile().x)), 0.0,
                     source.SamplePosition(source.TileFirstSample(tile.Tile().z))});
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        lod.positions.emplace_back(float(i * layout.SampleSpacing()), tile.At(i * stride, k * stride),
                                                   float(k * layout.SampleSpacing()));
                for (int32_t k = 0; k < n; ++k)
                    for (int32_t i = 0; i < n; ++i)
                    {
                        const uint32_t a = uint32_t(k * (n + 1) + i), b = a + 1, d = a + uint32_t(n + 1), c = d + 1;
                        for (uint32_t id : {a, d, c, a, c, b})
                            lod.indices.push_back(id);
                    }
                lod.topologyHash = TerrainTopologyHash(lod.positions, lod.indices);
                const std::optional<double> deviation = TerrainLodDeviation(source, tile, lod);
                REQUIRE(deviation);
                lod.maxDeviation = *deviation;
                const auto [lo, hi] = std::minmax_element(lod.positions.begin(), lod.positions.end(),
                                                          [](const glm::vec3 &p, const glm::vec3 &q) { return p.y < q.y; });

                TerrainTileRecord record;
                record.tile = tile.Tile();
                record.layer = level;
                record.level = level;
                record.kind = TerrainPayloadKind::LodHeight;
                record.source = false;
                record.samplesPerSide = n + 1;
                record.sampleSpacing = layout.SampleSpacing();
                record.minHeight = lo->y;
                record.maxHeight = hi->y;
                record.validExtent = *layout.ValidExtent(tile.Tile());
                record.maxDeviation = lod.maxDeviation;
                record.sourceChecksum = lod.sourceChecksum;
                record.topologyHash = lod.topologyHash;
                record.vertexCount = uint32_t(lod.positions.size());
                record.indexCount = uint32_t(lod.indices.size());
                writes.push_back({record, EncodeTerrainLod(lod)});
                record.kind = TerrainPayloadKind::Normal;
                writes.push_back(
                    {record, EncodeTerrainNormals(lod, std::vector<glm::vec3>(lod.positions.size(), {0.0f, 1.0f, 0.0f}))});
            }
        return writes;
    }

    TerrainStreamingSettings::View ViewSettings(uint64_t prefetchBudget = uint64_t(1) << 30)
    {
        TerrainStreamingSettings::View view;
        view.refineErrorPixels = 1.0;
        view.mergeErrorPixels = 0.75;
        view.prefetchErrorPixels = 0.5;
        view.prefetchBudgetBytes = prefetchBudget;
        view.meshRetainSeconds = 1.0;
        return view;
    }

    // Tiles of three LODs and four Region levels; built once per process.
    const TerrainEncodedRelease &SelectionRelease(const TerrainReleaseInfo &info)
    {
        static const TerrainEncodedRelease encoded = [&info]
        {
            std::vector<TerrainLayout> layers;
            for (double spacing : {4.0, 8.0, 16.0})
                layers.push_back(*TerrainLayout::CreateFromTileSize(kTileSize, spacing, kBounds));
            const std::vector<TileHeights> tiles = ReliefTiles(layers[0]);
            std::vector<TerrainTileWrite> sources;
            for (const TileHeights &tile : tiles)
                sources.push_back({&tile});
            const std::vector<TerrainPayloadWrite> lods = BuildTestLods(layers, tiles);
            const TerrainRegionWrites regions = BuildTestRegions(layers[0], tiles, 2, 4);
            TerrainResult<TerrainEncodedRelease> result = EncodeTerrainRelease(info, layers, sources, lods, regions);
            INFO(result.error.message);
            REQUIRE(result);
            return std::move(*result.value);
        }();
        return encoded;
    }

    struct Fixture
    {
        AssetId asset = TerrainAsset("terrain/selection.terrain.json");
        TerrainReleaseInfo info = Info(asset, 0x125);
        std::shared_ptr<TerrainMemorySource> source;
        std::shared_ptr<TerrainResidentRelease> release;
        TerrainPlacement placement;

        explicit Fixture(glm::dvec3 at = {1000.5, -20.0, -2000.25})
        {
            source = MakeTerrainMemorySource(SelectionRelease(info));
            release = TerrainResidentRelease::Create(OpenOk(TerrainReleaseHandle::Open(source)));
            placement = {asset, *WorldPosition::FromDoubleMeters(at)};
        }

        const TerrainIndex &Index() const { return release->Handle().Index(); }
        const TerrainRegionGrid &Grid() const { return *Index().regionGrid; }
        TerrainSelectionInstance Instance() const { return {release->SharedHandle(), placement}; }

        WorldPosition At(glm::dvec3 local) const
        {
            return *TryTerrainLocalToWorld(*TerrainLocalPosition::FromMeters(local), placement.translation);
        }

        // The unit's mesh area in terrain-local x, z.
        TerrainLocalBounds Area(const TerrainDrawUnit &unit) const
        {
            const TerrainLayout &layout = Index().layout;
            if (unit.kind == Kind::Tile)
            {
                const TileCoord t = unit.geometry.coord;
                return {layout.SamplePosition(layout.TileFirstSample(t.x)),
                        layout.SamplePosition(layout.TileFirstSample(t.z)),
                        layout.SamplePosition(layout.TileFirstSample(t.x) + layout.IntervalsPerTile()),
                        layout.SamplePosition(layout.TileFirstSample(t.z) + layout.IntervalsPerTile())};
            }
            const SampleRange domain =
                Grid().Domain({unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
            return {layout.SamplePosition(domain.min.x), layout.SamplePosition(domain.min.z),
                    layout.SamplePosition(domain.max.x), layout.SamplePosition(domain.max.z)};
        }

        std::vector<TerrainPayloadKey> AllKeys() const
        {
            std::vector<TerrainPayloadKey> keys;
            for (const std::vector<TerrainTileRecord> *records : {&Index().tiles, &Index().regions})
                for (const TerrainTileRecord &record : *records)
                    if (record.kind != TerrainPayloadKind::Height)
                        keys.push_back(TerrainPayloadKeyOf(info, record));
            return keys;
        }
    };

    std::vector<glm::dvec2> Probes()
    {
        std::vector<glm::dvec2> probes;
        for (int k = 0; kBounds.minZ + 1.37 + 6.71 * k < kBounds.maxZ; ++k)
            for (int i = 0; kBounds.minX + 0.93 + 7.13 * i < kBounds.maxX; ++i)
                probes.push_back({kBounds.minX + 0.93 + 7.13 * i, kBounds.minZ + 1.37 + 6.71 * k});
        return probes;
    }

    std::vector<int> CoverCounts(const Fixture &f, std::span<const TerrainDrawUnit> draws,
                                 std::span<const glm::dvec2> probes)
    {
        std::vector<int> counts(probes.size(), 0);
        for (const TerrainDrawUnit &unit : draws)
        {
            const TerrainLocalBounds area = f.Area(unit);
            for (size_t i = 0; i < probes.size(); ++i)
                if (probes[i].x > area.minX && probes[i].x < area.maxX && probes[i].y > area.minZ &&
                    probes[i].y < area.maxZ)
                    ++counts[i];
        }
        return counts;
    }

    void CheckCoveredOnce(const Fixture &f, std::span<const TerrainDrawUnit> draws)
    {
        const std::vector<glm::dvec2> probes = Probes();
        const std::vector<int> counts = CoverCounts(f, draws, probes);
        const auto wrong = std::find_if(counts.begin(), counts.end(), [](int c) { return c != 1; });
        if (wrong != counts.end())
        {
            const glm::dvec2 at = probes[size_t(wrong - counts.begin())];
            UNSCOPED_INFO("point " << at.x << ", " << at.y << " is drawn " << *wrong << " times");
        }
        CHECK(wrong == counts.end());
    }

    void CheckCoveredAtMostOnce(const Fixture &f, std::span<const TerrainDrawUnit> draws)
    {
        const std::vector<glm::dvec2> probes = Probes();
        const std::vector<int> counts = CoverCounts(f, draws, probes);
        CHECK(std::all_of(counts.begin(), counts.end(), [](int c) { return c <= 1; }));
    }

    glm::mat4 Everything()
    {
        glm::mat4 m(0.0f);
        m[3] = glm::vec4(0.0f, 0.0f, 0.5f, 1.0f);
        return m;
    }

    // Reverse-Z, camera-relative, as the renderer builds it.
    glm::mat4 Perspective(const glm::dvec3 &forward, double fovYDegrees, double aspect, double nearZ, double farZ)
    {
        const glm::vec3 f = glm::normalize(glm::vec3(forward));
        const glm::vec3 up = std::abs(f.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), f, up);
        const float focal = 1.0f / std::tan(float(glm::radians(fovYDegrees)) * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = focal / float(aspect);
        proj[1][1] = -focal;
        proj[2][3] = -1.0f;
        if (farZ > 0.0)
        {
            proj[2][2] = float(nearZ / (farZ - nearZ));
            proj[3][2] = float(farZ * nearZ / (farZ - nearZ));
        }
        else
            proj[3][2] = float(nearZ);
        return proj * view;
    }

    double Focal(double fovYDegrees, double heightPixels)
    {
        return heightPixels / (2.0 * std::tan(glm::radians(fovYDegrees) * 0.5));
    }

    TerrainView LookingEverywhere(WorldPosition eye, double focal)
    {
        return {{eye, Everything()}, focal, 0.0};
    }

    TerrainView Looking(WorldPosition eye, glm::dvec3 forward, double fovYDegrees = 60.0, double heightPixels = 720.0,
                        double farZ = 0.0)
    {
        return {{eye, Perspective(forward, fovYDegrees, 16.0 / 9.0, 0.1, farZ)}, Focal(fovYDegrees, heightPixels), farZ};
    }

    struct ReadySet
    {
        std::unordered_set<TerrainPayloadKey> keys;

        TerrainUnitReady Predicate() const
        {
            return [this](const TerrainDrawUnit &unit) { return keys.contains(unit.geometry) && keys.contains(unit.normals); };
        }
    };

    TerrainUnitReady AllReady()
    {
        return [](const TerrainDrawUnit &) { return true; };
    }

    size_t CountKind(std::span<const TerrainDrawUnit> draws, Kind kind)
    {
        return size_t(std::count_if(draws.begin(), draws.end(), [&](const TerrainDrawUnit &u) { return u.kind == kind; }));
    }

    const TerrainDrawUnit *TileDrawn(std::span<const TerrainDrawUnit> draws, TileCoord tile)
    {
        for (const TerrainDrawUnit &unit : draws)
            if (unit.kind == Kind::Tile && unit.geometry.coord == tile)
                return &unit;
        return nullptr;
    }

    bool KeyLess(const TerrainPayloadKey &a, const TerrainPayloadKey &b)
    {
        return std::tie(a.kind, a.level, a.layer, a.coord.x, a.coord.z) <
               std::tie(b.kind, b.level, b.layer, b.coord.x, b.coord.z);
    }

    using KeyOrder = bool (*)(const TerrainPayloadKey &, const TerrainPayloadKey &);

    std::map<TerrainPayloadKey, TerrainPayloadWant, KeyOrder> WantsByKey(std::span<const TerrainPayloadWant> wants)
    {
        std::map<TerrainPayloadKey, TerrainPayloadWant, KeyOrder> byKey(&KeyLess);
        for (const TerrainPayloadWant &want : wants)
            CHECK(byKey.emplace(want.key, want).second);
        return byKey;
    }

    void CheckWithinBudget(const TerrainStreamer &streamer)
    {
        const TerrainStreamingStats stats = streamer.Stats();
        CHECK(stats.residentBytes + stats.loadingBytes <= stats.budgetBytes);
    }
}

TEST_CASE("A selection with every unit ready covers the map exactly once from any eye", "[terrain][selection]")
{
    const Fixture f;
    TerrainViewSelector selector(ViewSettings());
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eyes[]{{30.0, 80.0, 100.0},  {-190.0, 40.0, -130.0}, {300.0, 600.0, 370.0},
                            {50.0, 3000.0, 120.0}, {-600.0, 30.0, 120.0},  {56.0, 1.0, 120.0}};
    for (const glm::dvec3 &eye : eyes)
        for (double focal : {20.0, 200.0, 2000.0})
        {
            INFO("eye " << eye.x << ", " << eye.y << ", " << eye.z << " focal " << focal);
            selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), AllReady());
            CheckCoveredOnce(f, selector.Draws());
            const TerrainViewStats &stats = selector.Stats();
            CHECK(stats.missing == 0);
            CHECK(stats.coarserFallbacks == 0);
            CHECK(stats.otherFallbacks == 0);
            CHECK(stats.drawnTiles + stats.drawnRegions == selector.Draws().size());
        }
}

TEST_CASE("Detail follows projected error: near ground is fine and far ground coarse", "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    TerrainViewSelector selector(ViewSettings());
    const glm::dvec3 eye{-180.0, 12.0, -120.0};
    selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), 200.0), AllReady());
    const std::span<const TerrainDrawUnit> draws = selector.Draws();
    CHECK(CountKind(draws, Kind::Tile) > 0);
    CHECK(CountKind(draws, Kind::Region) > 0);

    const TileCoord under{-3, -2};
    const TerrainDrawUnit *near = TileDrawn(draws, under);
    REQUIRE(near);
    CHECK(near->geometry.level == 0);
    const auto farthest = std::max_element(draws.begin(), draws.end(), [](const auto &a, const auto &b) {
        return a.distance < b.distance;
    });
    CHECK((farthest->kind == Kind::Region || farthest->geometry.level > 0));

    for (const TerrainDrawUnit &unit : draws)
    {
        INFO("unit at distance " << unit.distance);
        if (unit.kind == Kind::Tile && unit.geometry.level == 0)
            continue;
        CHECK(unit.errorPixels <= 1.0);
    }

    TerrainViewSelector zoomed(ViewSettings());
    zoomed.Update({&instance, 1}, LookingEverywhere(f.At(eye), 2000.0), AllReady());
    CHECK(zoomed.Draws().size() > draws.size());
    CHECK(CountKind(zoomed.Draws(), Kind::Tile) > CountKind(draws, Kind::Tile));
}

TEST_CASE("Hysteresis keeps a tile's LOD between the merge and refine bounds", "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    TerrainViewSelector selector(ViewSettings());
    const TileCoord tile{1, 2};
    const glm::dvec3 eye{kTileSize * 1.5, 140.0, kTileSize * 2.5};

    std::vector<double> focals;
    for (double focal = 10.0; focal < 4000.0; focal *= 1.15)
        focals.push_back(focal);
    std::map<double, uint32_t> rising, falling;
    for (double focal : focals)
    {
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), AllReady());
        if (const TerrainDrawUnit *unit = TileDrawn(selector.Draws(), tile))
            rising[focal] = unit->geometry.level;
    }
    for (auto it = focals.rbegin(); it != focals.rend(); ++it)
    {
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), *it), AllReady());
        if (const TerrainDrawUnit *unit = TileDrawn(selector.Draws(), tile))
            falling[*it] = unit->geometry.level;
    }
    REQUIRE(rising.size() > 4);

    std::optional<uint32_t> previous;
    for (const auto &[focal, level] : rising)
    {
        if (previous)
            CHECK(level <= *previous);
        previous = level;
    }
    CHECK(rising.begin()->second > rising.rbegin()->second);

    bool lagged = false;
    for (const auto &[focal, level] : falling)
        if (const auto up = rising.find(focal); up != rising.end())
        {
            CHECK(level <= up->second);
            lagged = lagged || level < up->second;
        }
    CHECK(lagged);

    const double still = focals[focals.size() / 2];
    selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), still), AllReady());
    const TerrainDrawUnit *first = TileDrawn(selector.Draws(), tile);
    REQUIRE(first);
    const uint32_t settled = first->geometry.level;
    for (int i = 0; i < 5; ++i)
    {
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), still), AllReady());
        REQUIRE(TileDrawn(selector.Draws(), tile));
        CHECK(TileDrawn(selector.Draws(), tile)->geometry.level == settled);
    }
}

TEST_CASE("Turning and changing projection never drops a visible unit and never changes detail",
          "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eye{40.0, 90.0, 110.0};
    const double focal = Focal(60.0, 720.0);

    TerrainViewSelector all(ViewSettings());
    all.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), AllReady());
    const std::vector<TerrainDrawUnit> everything(all.Draws().begin(), all.Draws().end());

    std::vector<std::vector<glm::vec3>> vertices;
    for (const TerrainDrawUnit &unit : everything)
    {
        std::vector<glm::vec3> local;
        if (unit.kind == Kind::Tile)
        {
            const auto lod = f.release->Handle().ReadLod({f.asset, unit.geometry.coord}, unit.geometry.layer);
            REQUIRE(lod);
            for (const glm::vec3 &p : lod.value->positions)
                local.push_back(glm::vec3(glm::dvec3(p) + lod.value->origin.Meters() - eye));
        }
        else
        {
            const auto mesh =
                f.release->Handle().ReadRegion({unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
            REQUIRE(mesh);
            for (const glm::vec3 &p : mesh.value->positions)
                local.push_back(glm::vec3(glm::dvec3(p) + mesh.value->origin.Meters() - eye));
        }
        vertices.push_back(std::move(local));
    }

    TerrainViewSelector turning(ViewSettings());
    size_t checkedVisible = 0;
    for (int step = 0; step < 24; ++step)
        for (double fov : {20.0, 60.0, 100.0})
        {
            const double yaw = step * 15.0 * 3.14159265358979 / 180.0;
            const glm::dvec3 forward(std::cos(yaw), -0.35, std::sin(yaw));
            TerrainView view = Looking(f.At(eye), forward, fov, 720.0);
            view.focalPixels = focal;
            turning.Update({&instance, 1}, view, AllReady());
            const std::span<const TerrainDrawUnit> draws = turning.Draws();
            CheckCoveredAtMostOnce(f, draws);
            std::set<TerrainPayloadKey, KeyOrder> drawn(&KeyLess);
            for (const TerrainDrawUnit &unit : draws)
                drawn.insert(unit.geometry);
            for (size_t u = 0; u < everything.size(); ++u)
            {
                const bool visible = std::any_of(vertices[u].begin(), vertices[u].end(), [&](const glm::vec3 &p) {
                    const glm::vec4 clip = view.frustum.viewProj * glm::vec4(p, 1.0f);
                    return std::abs(clip.x) < clip.w && std::abs(clip.y) < clip.w && clip.z > 0.0f && clip.z < clip.w;
                });
                if (!visible)
                    continue;
                ++checkedVisible;
                INFO("yaw step " << step << " fov " << fov << " unit " << u);
                CHECK(drawn.contains(everything[u].geometry));
            }
            for (const TerrainDrawUnit &unit : draws)
                CHECK(std::any_of(everything.begin(), everything.end(),
                                  [&](const TerrainDrawUnit &e) { return e.geometry == unit.geometry; }));
        }
    CHECK(checkedVisible > 100);
}

TEST_CASE("Units that are not ready give way to ready coarser or finer ones and leave holes only where nothing is",
          "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eye{-150.0, 30.0, -100.0};
    const double focal = 300.0;
    std::vector<TerrainPayloadKey> keys = f.AllKeys();

    TerrainViewSelector reference(ViewSettings());
    reference.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), AllReady());
    REQUIRE(CountKind(reference.Draws(), Kind::Tile) > 0);
    const bool levelZeroDrawn = std::any_of(reference.Draws().begin(), reference.Draws().end(), [](const auto &u) {
        return u.kind == Kind::Tile && u.geometry.level == 0;
    });
    REQUIRE(levelZeroDrawn);

    SECTION("A missing LOD is drawn at another ready LOD of the tile")
    {
        ReadySet ready;
        for (const TerrainPayloadKey &key : keys)
            if (key.kind == TerrainPayloadKind::Region || key.level != 0)
                ready.keys.insert(key);
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), ready.Predicate());
        CheckCoveredOnce(f, selector.Draws());
        CHECK(selector.Stats().otherFallbacks > 0);
        CHECK(selector.Stats().missing == 0);
        for (const TerrainDrawUnit &unit : selector.Draws())
            CHECK((unit.kind == Kind::Region || unit.geometry.level != 0));
    }
    SECTION("Regions stand in while their tiles load")
    {
        ReadySet ready;
        for (const TerrainPayloadKey &key : keys)
            if (key.kind == TerrainPayloadKind::Region)
                ready.keys.insert(key);
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), ready.Predicate());
        CheckCoveredOnce(f, selector.Draws());
        CHECK(CountKind(selector.Draws(), Kind::Tile) == 0);
        CHECK(selector.Stats().coarserFallbacks > 0);
        const auto wants = WantsByKey(selector.Wants());
        for (const TerrainDrawUnit &unit : reference.Draws())
        {
            REQUIRE(wants.contains(unit.geometry));
            CHECK(wants.at(unit.geometry).demand == Demand::Visible);
        }
    }
    SECTION("Nothing ready draws nothing and wants the coarsest Region first")
    {
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), ReadySet{}.Predicate());
        CHECK(selector.Draws().empty());
        CHECK(selector.Stats().missing > 0);
        const uint32_t top = f.Grid().Levels() - 1;
        const TerrainPayloadKey root = TerrainPayloadKeyOf(f.info, *f.Index().FindRegion({top, 0, 0}));
        const auto wants = WantsByKey(selector.Wants());
        REQUIRE(wants.contains(root));
        CHECK(wants.at(root).demand == Demand::Visible);
        for (const TerrainPayloadWant &want : selector.Wants())
            if (!(want.key == root) && want.demand == Demand::Visible)
                CHECK(wants.at(root).rank < want.rank);
    }
    SECTION("Only the root ready draws the root alone")
    {
        ReadySet ready;
        const uint32_t top = f.Grid().Levels() - 1;
        ready.keys.insert(TerrainPayloadKeyOf(f.info, *f.Index().FindRegion({top, 0, 0})));
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal), ready.Predicate());
        REQUIRE(selector.Draws().size() == 1);
        CHECK(selector.Draws()[0].geometry.level == top);
        CheckCoveredOnce(f, selector.Draws());
    }
    SECTION("Moving away keeps the tiles until their Region arrives")
    {
        ReadySet ready;
        for (const TerrainPayloadKey &key : keys)
            if (key.kind != TerrainPayloadKind::Region)
                ready.keys.insert(key);
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), 4000.0), ready.Predicate());
        CheckCoveredOnce(f, selector.Draws());
        REQUIRE(CountKind(selector.Draws(), Kind::Region) == 0);
        size_t fallbacks = 0;
        for (double focal2 = 4000.0; focal2 > 5.0; focal2 *= 0.8)
        {
            selector.Update({&instance, 1}, LookingEverywhere(f.At(eye), focal2), ready.Predicate());
            CheckCoveredAtMostOnce(f, selector.Draws());
            CHECK(CountKind(selector.Draws(), Kind::Region) == 0);
            fallbacks += selector.Stats().otherFallbacks;
        }
        CHECK(fallbacks > 0);
    }
}

TEST_CASE("Wants are Visible for what the view draws and chooses, Prefetch for the rest within the budget",
          "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eye{-150.0, 40.0, -100.0};
    const TerrainView view = Looking(f.At(eye), {1.0, -0.3, 1.0}, 50.0, 720.0);

    TerrainViewSelector selector(ViewSettings());
    selector.Update({&instance, 1}, view, AllReady());
    const auto wants = WantsByKey(selector.Wants());
    for (const TerrainDrawUnit &unit : selector.Draws())
    {
        REQUIRE(wants.contains(unit.geometry));
        CHECK(wants.at(unit.geometry).demand == Demand::Visible);
        CHECK(wants.at(unit.geometry).rank == float(unit.distance));
        if (unit.kind == Kind::Tile)
        {
            REQUIRE(wants.contains(unit.normals));
            CHECK(wants.at(unit.normals).demand == Demand::Visible);
        }
    }
    const TerrainViewStats &stats = selector.Stats();
    CHECK(stats.culled > 0);
    CHECK(stats.prefetchWants > 0);
    CHECK(stats.prefetchDropped == 0);

    TerrainViewSelector everywhere(ViewSettings());
    everywhere.Update({&instance, 1}, LookingEverywhere(f.At(eye), view.focalPixels), AllReady());
    for (const TerrainDrawUnit &unit : everywhere.Draws())
    {
        REQUIRE(wants.contains(unit.geometry));
        const bool drawnHere = std::any_of(selector.Draws().begin(), selector.Draws().end(),
                                           [&](const TerrainDrawUnit &u) { return u.geometry == unit.geometry; });
        CHECK(wants.at(unit.geometry).demand == (drawnHere ? Demand::Visible : Demand::Prefetch));
    }
    size_t finer = 0;
    for (const TerrainPayloadWant &want : selector.Wants())
        if (want.demand == Demand::Prefetch && want.key.kind == TerrainPayloadKind::LodHeight &&
            std::none_of(everywhere.Draws().begin(), everywhere.Draws().end(),
                         [&](const TerrainDrawUnit &u) { return u.geometry == want.key; }))
            ++finer;
    CHECK(finer > 0);

    const uint64_t budget = stats.prefetchBytes / 3;
    TerrainViewSelector limited(ViewSettings(budget));
    limited.Update({&instance, 1}, view, AllReady());
    CHECK(limited.Stats().prefetchBytes <= budget);
    CHECK(limited.Stats().prefetchDropped > 0);
    CHECK(limited.Stats().visibleWants == stats.visibleWants);
    uint64_t prefetched = 0;
    for (const TerrainPayloadWant &want : limited.Wants())
        if (want.demand == Demand::Prefetch)
            prefetched += TerrainPayloadFootprint(*f.Index().Find(want.key.coord, want.key.layer, want.key.kind, want.key.level));
    CHECK(prefetched == limited.Stats().prefetchBytes);

    TerrainViewSelector none(ViewSettings(0));
    none.Update({&instance, 1}, view, AllReady());
    CHECK(none.Stats().prefetchWants == 0);
    CHECK(std::none_of(none.Wants().begin(), none.Wants().end(),
                       [](const TerrainPayloadWant &w) { return w.demand == Demand::Prefetch; }));
}

TEST_CASE("Nothing past the far plane is drawn or wanted", "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eye{-190.0, 20.0, -130.0};
    const double farZ = 150.0;
    TerrainViewSelector selector(ViewSettings());
    selector.Update({&instance, 1}, Looking(f.At(eye), {1.0, -0.2, 1.0}, 60.0, 720.0, farZ), AllReady());
    REQUIRE_FALSE(selector.Draws().empty());
    for (const TerrainDrawUnit &unit : selector.Draws())
        CHECK(unit.distance <= farZ);
    for (const TerrainPayloadWant &want : selector.Wants())
        CHECK(want.rank <= float(farZ));
}

TEST_CASE("Another volume gets the view's detail, so shadow casters match what is drawn", "[terrain][selection]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    const glm::dvec3 eye{-100.0, 50.0, -60.0};
    TerrainViewSelector selector(ViewSettings());
    selector.Update({&instance, 1}, Looking(f.At(eye), {1.0, -0.4, 0.6}, 45.0, 720.0), AllReady());
    const std::vector<TerrainPayloadWant> before(selector.Wants().begin(), selector.Wants().end());

    std::vector<TerrainDrawUnit> casters;
    selector.Collect({f.At({300.0, 900.0, 300.0}), Everything()}, AllReady(), casters);
    CheckCoveredOnce(f, casters);
    for (const TerrainDrawUnit &unit : selector.Draws())
        CHECK(std::any_of(casters.begin(), casters.end(),
                          [&](const TerrainDrawUnit &c) { return c.geometry == unit.geometry; }));
    CHECK(casters.size() > selector.Draws().size());
    CHECK(selector.Wants().size() == before.size());
}

TEST_CASE("Two viewports pick different detail for the same ground and share one read per payload",
          "[terrain][selection][streaming]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    auto executor = std::make_shared<TerrainManualExecutor>();
    ManualClock clock;
    TerrainStreamer streamer(StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Interest), executor,
                             clock.Function());
    REQUIRE(streamer.Add(f.release) == TerrainResidencyMode::Interest);
    const TerrainUnitReady ready = [&](const TerrainDrawUnit &unit) {
        return streamer.Status(unit.geometry).state == TerrainPayloadState::Ready &&
               streamer.Status(unit.normals).state == TerrainPayloadState::Ready;
    };

    const glm::dvec3 eye{60.0, 70.0, 130.0};
    TerrainViewSelector wide(ViewSettings()), narrow(ViewSettings());
    const TerrainInterestId wideId = streamer.AddInterest(), narrowId = streamer.AddInterest();
    const TerrainView wideView = Looking(f.At(eye), {0.2, -0.5, 1.0}, 90.0, 360.0);
    const TerrainView narrowView = Looking(f.At(eye), {0.2, -0.5, 1.0}, 25.0, 1440.0);
    const size_t opened = f.source->Requests().size();
    std::unordered_set<TerrainPayloadKey> distinct;
    for (int frame = 0; frame < 40; ++frame)
    {
        wide.Update({&instance, 1}, wideView, ready);
        narrow.Update({&instance, 1}, narrowView, ready);
        CheckCoveredAtMostOnce(f, wide.Draws());
        CheckCoveredAtMostOnce(f, narrow.Draws());
        for (const std::span<const TerrainPayloadWant> wants : {wide.Wants(), narrow.Wants()})
            for (const TerrainPayloadWant &want : wants)
                distinct.insert(want.key);
        streamer.SetInterest(wideId, wide.Wants());
        streamer.SetInterest(narrowId, narrow.Wants());
        streamer.Update();
        executor->RunAll();
        clock.Advance(1.0 / 60.0);
    }
    CHECK(wide.Stats().missing == 0);
    CHECK(narrow.Stats().missing == 0);
    CHECK(wide.Stats().coarserFallbacks + wide.Stats().otherFallbacks == 0);
    CHECK(narrow.Stats().coarserFallbacks + narrow.Stats().otherFallbacks == 0);

    size_t differ = 0;
    for (const TerrainDrawUnit &unit : narrow.Draws())
        if (unit.kind == Kind::Tile)
            if (const TerrainDrawUnit *other = TileDrawn(wide.Draws(), unit.geometry.coord);
                other && other->geometry.level != unit.geometry.level)
                ++differ;
    CHECK(differ > 0);

    std::unordered_set<TerrainPayloadKey> wantedByWide;
    for (const TerrainPayloadWant &want : wide.Wants())
        wantedByWide.insert(want.key);
    const size_t shared = size_t(std::count_if(narrow.Wants().begin(), narrow.Wants().end(),
                                               [&](const TerrainPayloadWant &w) { return wantedByWide.contains(w.key); }));
    CHECK(shared > 0);
    const TerrainStreamingStats stats = streamer.Stats();
    CHECK(stats.evicted == 0);
    CHECK(stats.readsStarted == f.source->Requests().size() - opened);
    CHECK(stats.readsStarted <= distinct.size());
    CHECK(stats.readsFailed == 0);
    std::set<std::pair<std::string, uint64_t>> ranges;
    for (size_t i = opened; i < f.source->Requests().size(); ++i)
        CHECK(ranges.insert({f.source->Requests()[i].file, f.source->Requests()[i].offset}).second);
}

TEST_CASE("Streaming while moving keeps every frame covered at most once and settles to full coverage",
          "[terrain][selection][streaming]")
{
    const Fixture f;
    const TerrainSelectionInstance instance = f.Instance();
    uint64_t total = 0;
    for (const TerrainPayloadKey &key : f.AllKeys())
        total += TerrainPayloadFootprint(*f.Index().Find(key.coord, key.layer, key.kind, key.level));
    auto executor = std::make_shared<TerrainManualExecutor>();
    ManualClock clock;
    TerrainStreamer streamer(StreamingSettings(total / 2, TerrainResidencyMode::Interest), executor, clock.Function());
    REQUIRE(streamer.Add(f.release) == TerrainResidencyMode::Interest);
    const TerrainUnitReady ready = [&](const TerrainDrawUnit &unit) {
        return streamer.Status(unit.geometry).state == TerrainPayloadState::Ready &&
               streamer.Status(unit.normals).state == TerrainPayloadState::Ready;
    };
    TerrainViewSelector selector(ViewSettings(total / 8));
    const TerrainInterestId id = streamer.AddInterest();
    const auto frame = [&](const TerrainView &view) {
        selector.Update({&instance, 1}, view, ready);
        CheckCoveredAtMostOnce(f, selector.Draws());
        streamer.SetInterest(id, selector.Wants());
        streamer.Update();
        executor->RunAll();
        clock.Advance(1.0 / 60.0);
        CheckWithinBudget(streamer);
    };

    const double focal = 400.0;
    for (int step = 0; step <= 60; ++step)
    {
        const double t = step / 60.0;
        const glm::dvec3 eye{-190.0 + 480.0 * t, 600.0 - 580.0 * t, -130.0 + 480.0 * t};
        frame(LookingEverywhere(f.At(eye), focal));
    }
    for (int settle = 0; settle < 20; ++settle)
        frame(LookingEverywhere(f.At({290.0, 20.0, 350.0}), focal));
    CheckCoveredOnce(f, selector.Draws());
    CHECK(selector.Stats().missing == 0);
    CHECK(selector.Stats().coarserFallbacks + selector.Stats().otherFallbacks == 0);
}

TEST_CASE("A far placement selects the same units as one near the origin", "[terrain][selection][far]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const Fixture near;
    const Fixture far({150000.5, -20.0, -75000.25});
    const glm::dvec3 eye{30.0, 45.0, 60.0};
    const auto select = [&](const Fixture &f) {
        const TerrainSelectionInstance instance = f.Instance();
        TerrainViewSelector selector(ViewSettings());
        selector.Update({&instance, 1}, Looking(f.At(eye), {0.6, -0.3, 1.0}, 60.0, 720.0), AllReady());
        std::vector<std::pair<TerrainPayloadKind, std::tuple<uint32_t, int32_t, int32_t>>> units;
        for (const TerrainDrawUnit &unit : selector.Draws())
            units.push_back({unit.geometry.kind, {unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z}});
        std::sort(units.begin(), units.end());
        return units;
    };
    const auto a = select(near), b = select(far);
    CHECK(!a.empty());
    CHECK(a == b);
}
