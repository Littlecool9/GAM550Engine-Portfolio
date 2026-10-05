/**
 * @file TerrainTileMesh.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainTileMesh.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainTileMesh.h>

#include "TerrainPackTestSupport.h"

#include <core/ecs/components/WorldTransform.h>
#include <terrain/TileHeights.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mir;

namespace
{
    constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

    // About 5 m of twist in a 4 m cell.
    double Hills(double x, double z)
    {
        return 180.0 + 40.0 * std::sin(0.011 * x + 0.7) * std::cos(0.017 * z - 0.4) +
               6.0 * std::sin(0.09 * x + 0.13 * z) + 3.0 * std::sin(0.37 * x) * std::sin(0.29 * z);
    }

    TerrainLocalPosition Local(double x, double z)
    {
        return *TerrainLocalPosition::FromMeters({x, 0.0, z});
    }

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    // Filled from global positions, so shared edge samples are equal.
    class Tiles
    {
    public:
        Tiles(const TerrainLayout &layout, double (*height)(double, double)) : mLayout(layout)
        {
            const TileRange range = layout.Tiles();
            const int32_t n = layout.IntervalsPerTile();
            for (int32_t tz = range.min.z; tz <= range.max.z; ++tz)
                for (int32_t tx = range.min.x; tx <= range.max.x; ++tx)
                {
                    std::optional<TileHeights> block = TileHeights::Create({tx, tz}, n);
                    REQUIRE(block);
                    const int64_t i0 = layout.TileFirstSample(tx);
                    const int64_t k0 = layout.TileFirstSample(tz);
                    for (int32_t k = 0; k <= n; ++k)
                        for (int32_t i = 0; i <= n; ++i)
                            block->Set(i, k, static_cast<float>(height(layout.SamplePosition(i0 + i),
                                                                       layout.SamplePosition(k0 + k))));
                    mTiles.emplace(TileCoord{tx, tz}, std::move(*block));
                }
        }

        TileSampleLookup Lookup() const
        {
            return [this](TileCoord tile)
            {
                const auto it = mTiles.find(tile);
                return it == mTiles.end() ? std::span<const float>{} : it->second.Samples();
            };
        }

        float At(TileCoord tile, GlobalSampleIndex sample) const
        {
            const TileHeights &block = mTiles.at(tile);
            return block.At(static_cast<int32_t>(sample.x - mLayout.TileFirstSample(tile.x)),
                            static_cast<int32_t>(sample.z - mLayout.TileFirstSample(tile.z)));
        }

        void Set(TileCoord tile, GlobalSampleIndex sample, float value)
        {
            mTiles.at(tile).Set(static_cast<int32_t>(sample.x - mLayout.TileFirstSample(tile.x)),
                                static_cast<int32_t>(sample.z - mLayout.TileFirstSample(tile.z)), value);
        }

        void Erase(TileCoord tile) { mTiles.erase(tile); }

        double MaxAbsHeight() const
        {
            double result = 0.0;
            for (const auto &[tile, block] : mTiles)
                for (const float h : block.Samples())
                    result = std::max(result, std::abs(double(h)));
            return result;
        }

    private:
        TerrainLayout mLayout;
        std::unordered_map<TileCoord, TileHeights> mTiles;
    };

    // A function: TerrainAsset REQUIREs, which needs a running test.
    TerrainReleaseInfo MeshRelease()
    {
        return test::Info(test::TerrainAsset("terrain/mesh.terrain.json"), 0x14a11e5u);
    }

    TerrainLayout Block(TileCoord min, double spacing = 4.0, int32_t n = 64)
    {
        const double t = spacing * n;
        const auto layout =
            TerrainLayout::Create(spacing, n, {min.x * t, min.z * t, (min.x + 2) * t, (min.z + 2) * t});
        REQUIRE(layout);
        return *layout;
    }

    constexpr TileCoord kBlocks[]{{-1, -1}, {-600, 585}};

    TerrainTileMesh BuildOk(const TerrainLayout &layout, TileCoord tile, const TileSampleLookup &lookup)
    {
        TerrainMeshResult built = BuildTerrainTileMesh(layout, MeshRelease(), tile, lookup);
        INFO("tile " << tile.x << ", " << tile.z << " status " << int(built.status));
        REQUIRE(built.IsOk());
        REQUIRE(built.mesh);
        return std::move(*built.mesh);
    }

    int64_t Columns(const TerrainTileMesh &mesh) { return mesh.samples.max.x - mesh.samples.min.x + 1; }

    GlobalSampleIndex SampleOf(const TerrainTileMesh &mesh, size_t vertex)
    {
        const int64_t columns = Columns(mesh);
        return {mesh.samples.min.x + int64_t(vertex) % columns, mesh.samples.min.z + int64_t(vertex) / columns};
    }

    const TerrainMeshVertex &VertexAt(const TerrainTileMesh &mesh, GlobalSampleIndex sample)
    {
        const int64_t v = (sample.z - mesh.samples.min.z) * Columns(mesh) + (sample.x - mesh.samples.min.x);
        return mesh.vertices.at(static_cast<size_t>(v));
    }

    glm::dvec3 LocalOf(const TerrainTileMesh &mesh, const TerrainMeshVertex &vertex)
    {
        return mesh.origin.Meters() + glm::dvec3(vertex.position);
    }

    std::array<glm::dvec3, 3> TriangleOf(const TerrainTileMesh &mesh, size_t triangle)
    {
        std::array<glm::dvec3, 3> corners{};
        for (size_t v = 0; v < 3; ++v)
            corners[v] = LocalOf(mesh, mesh.vertices[mesh.indices[triangle * 3 + v]]);
        return corners;
    }

    glm::dvec3 ExpectedNormal(const TerrainLayout &layout, const Tiles &tiles, GlobalSampleIndex sample)
    {
        const TileRange range = layout.Tiles();
        const auto owner = [&](GlobalSampleIndex s)
        {
            return TileCoord{std::clamp(layout.TileOfCell(s.x), range.min.x, range.max.x),
                             std::clamp(layout.TileOfCell(s.z), range.min.z, range.max.z)};
        };
        const auto inLayout = [&](GlobalSampleIndex s)
        {
            return s.x >= layout.TileFirstSample(range.min.x) && s.x <= layout.TileFirstSample(range.max.x + 1) &&
                   s.z >= layout.TileFirstSample(range.min.z) && s.z <= layout.TileFirstSample(range.max.z + 1);
        };
        const auto pick = [&](GlobalSampleIndex s) { return inLayout(s) ? s : sample; };
        const GlobalSampleIndex l = pick({sample.x - 1, sample.z}), r = pick({sample.x + 1, sample.z});
        const GlobalSampleIndex d = pick({sample.x, sample.z - 1}), u = pick({sample.x, sample.z + 1});
        const double gx = (double(tiles.At(owner(r), r)) - tiles.At(owner(l), l)) /
                          (layout.SamplePosition(r.x) - layout.SamplePosition(l.x));
        const double gz = (double(tiles.At(owner(u), u)) - tiles.At(owner(d), d)) /
                          (layout.SamplePosition(u.z) - layout.SamplePosition(d.z));
        return glm::normalize(glm::dvec3(-gx, 1.0, -gz));
    }
}

TEST_CASE("A tile mesh has one vertex on every stored sample", "[terrain][mesh]")
{
    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const Tiles tiles(layout, Hills);

        for (int32_t dz = 0; dz <= 1; ++dz)
            for (int32_t dx = 0; dx <= 1; ++dx)
            {
                const TileCoord tile{min.x + dx, min.z + dz};
                const TerrainTileMesh mesh = BuildOk(layout, tile, tiles.Lookup());
                CHECK(mesh.release == MeshRelease());
                CHECK(mesh.surfaceVersion == kTerrainSurfaceVersion);
                CHECK(mesh.tile == tile);

                const int64_t i0 = layout.TileFirstSample(tile.x);
                const int64_t k0 = layout.TileFirstSample(tile.z);
                CHECK(mesh.origin.Meters() == glm::dvec3(layout.SamplePosition(i0), 0.0, layout.SamplePosition(k0)));
                CHECK(mesh.samples.min == layout.TileSamples(tile)->min);
                CHECK(mesh.samples.max == layout.TileSamples(tile)->max);
                REQUIRE(mesh.vertices.size() == 65u * 65u);
                CHECK(mesh.indices.size() == 64u * 64u * 6u);

                int mismatches = 0;
                for (size_t v = 0; v < mesh.vertices.size(); ++v)
                {
                    const GlobalSampleIndex sample = SampleOf(mesh, v);
                    const glm::vec3 &p = mesh.vertices[v].position;
                    // Stored floats bit for bit; 4 m steps are exact in float.
                    if (p.y != tiles.At(tile, sample) || p.x != float(sample.x - i0) * 4.0f ||
                        p.z != float(sample.z - k0) * 4.0f)
                        ++mismatches;
                }
                CHECK(mismatches == 0);
            }
    }
}

TEST_CASE("Tile mesh triangles are TriangulateCell's, counter-clockwise from above", "[terrain][mesh]")
{
    const TerrainLayout layout = Block({-1, -1});
    const Tiles tiles(layout, Hills);
    const TerrainTileMesh mesh = BuildOk(layout, {-1, 0}, tiles.Lookup());

    int mismatches = 0;
    int clockwise = 0;
    size_t at = 0;
    for (int64_t k = mesh.samples.min.z; k < mesh.samples.max.z; ++k)
        for (int64_t i = mesh.samples.min.x; i < mesh.samples.max.x; ++i)
            for (const std::array<GlobalSampleIndex, 3> &triangle : TriangulateCell({i, k}).triangles)
            {
                for (const GlobalSampleIndex corner : triangle)
                    if (&mesh.vertices.at(mesh.indices.at(at++)) != &VertexAt(mesh, corner))
                        ++mismatches;

                const std::array<glm::dvec3, 3> c = TriangleOf(mesh, at / 3 - 1);
                if (glm::cross(c[1] - c[0], c[2] - c[0]).y <= 0.0)
                    ++clockwise;
            }
    CHECK(at == mesh.indices.size());
    CHECK(mismatches == 0);
    CHECK(clockwise == 0);
}

TEST_CASE("Points on tile mesh triangles evaluate to the canonical surface", "[terrain][mesh]")
{
    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const Tiles tiles(layout, Hills);
        const TileSampleLookup lookup = tiles.Lookup();
        const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());

        std::vector<TerrainTileMesh> meshes;
        for (int32_t dz = 0; dz <= 1; ++dz)
            for (int32_t dx = 0; dx <= 1; ++dx)
                meshes.push_back(BuildOk(layout, {min.x + dx, min.z + dz}, lookup));

        std::mt19937_64 rng(0x14);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        double worstHeight = 0.0;
        double worstNormal = 0.0;
        int failures = 0;
        for (int probe = 0; probe < 20000; ++probe)
        {
            const TerrainTileMesh &mesh = meshes[probe % meshes.size()];
            const size_t triangle = std::uniform_int_distribution<size_t>(0, mesh.indices.size() / 3 - 1)(rng);
            const std::array<glm::dvec3, 3> c = TriangleOf(mesh, triangle);

            double w1 = unit(rng), w2 = unit(rng);
            if (w1 + w2 > 1.0)
                w1 = 1.0 - w1, w2 = 1.0 - w2;
            // A quarter on an edge, an eighth on a corner.
            const int kind = probe % 8;
            if (kind < 2)
                w2 = 0.0;
            else if (kind == 2)
                w1 = 1.0 - w2;
            else if (kind == 3)
                w1 = w2 = 0.0;
            const double w0 = 1.0 - w1 - w2;
            const glm::dvec3 point = w0 * c[0] + w1 * c[1] + w2 * c[2];

            const SurfaceSample s = EvaluateSurface(layout, Local(point.x, point.z), lookup);
            if (!s.IsOk())
            {
                ++failures;
                continue;
            }
            worstHeight = std::max(worstHeight, std::abs(s.height - point.y));
            if (kind >= 4)
                worstNormal = std::max(worstNormal,
                                       glm::length(s.normal - glm::normalize(glm::cross(c[1] - c[0], c[2] - c[0]))));
        }
        CHECK(failures == 0);
        CHECK(worstHeight <= tolerance);
        CHECK(worstNormal <= 1e-12);
    }
}

TEST_CASE("Neighbouring tile meshes share edge vertices and normals", "[terrain][mesh]")
{
    struct Case
    {
        TileCoord min;
        double spacing;
        int32_t n;
        double positionTolerance;
    };
    const Case cases[]{{{-1, -1}, 4.0, 64, 0.0}, {{-600, 585}, 4.0, 64, 0.0}, {{-3, 2}, 0.1, 160, 1e-6}};

    for (const Case &c : cases)
    {
        INFO("block " << c.min.x << ", " << c.min.z << " spacing " << c.spacing);
        const TerrainLayout layout = Block(c.min, c.spacing, c.n);
        const Tiles tiles(layout, Hills);
        std::unordered_map<TileCoord, TerrainTileMesh> meshes;
        for (int32_t dz = 0; dz <= 1; ++dz)
            for (int32_t dx = 0; dx <= 1; ++dx)
                meshes.emplace(TileCoord{c.min.x + dx, c.min.z + dz},
                               BuildOk(layout, {c.min.x + dx, c.min.z + dz}, tiles.Lookup()));

        const int64_t edgeX = layout.TileFirstSample(c.min.x + 1);
        const int64_t edgeZ = layout.TileFirstSample(c.min.z + 1);
        double worstPosition = 0.0;
        int normalMismatches = 0;
        int compared = 0;
        const auto compare = [&](TileCoord a, TileCoord b, GlobalSampleIndex sample)
        {
            const TerrainMeshVertex &va = VertexAt(meshes.at(a), sample);
            const TerrainMeshVertex &vb = VertexAt(meshes.at(b), sample);
            const glm::dvec3 d = LocalOf(meshes.at(a), va) - LocalOf(meshes.at(b), vb);
            worstPosition = std::max({worstPosition, std::abs(d.x), std::abs(d.y), std::abs(d.z)});
            if (va.position.y != vb.position.y || va.normal != vb.normal)
                ++normalMismatches;
            ++compared;
        };

        for (int32_t row = 0; row <= 1; ++row)
            for (int64_t s = 0; s <= c.n; ++s)
            {
                const int64_t k = layout.TileFirstSample(c.min.z + row) + s;
                compare({c.min.x, c.min.z + row}, {c.min.x + 1, c.min.z + row}, {edgeX, k});
                const int64_t i = layout.TileFirstSample(c.min.x + row) + s;
                compare({c.min.x + row, c.min.z}, {c.min.x + row, c.min.z + 1}, {i, edgeZ});
            }
        for (int32_t t = 1; t < 4; ++t)
            compare({c.min.x, c.min.z}, {c.min.x + t % 2, c.min.z + t / 2}, {edgeX, edgeZ});

        CHECK(compared == 4 * (c.n + 1) + 3);
        CHECK(worstPosition <= c.positionTolerance);
        CHECK(normalMismatches == 0);
    }
}

TEST_CASE("Tile mesh normals are central differences across tile edges", "[terrain][mesh]")
{
    const TerrainLayout layout = Block({-1, -1});
    const Tiles tiles(layout, Hills);

    double worst = 0.0;
    for (int32_t dz = 0; dz <= 1; ++dz)
        for (int32_t dx = 0; dx <= 1; ++dx)
        {
            const TerrainTileMesh mesh = BuildOk(layout, {-1 + dx, -1 + dz}, tiles.Lookup());
            for (size_t v = 0; v < mesh.vertices.size(); ++v)
            {
                const glm::dvec3 expected = ExpectedNormal(layout, tiles, SampleOf(mesh, v));
                worst = std::max(worst, glm::length(glm::dvec3(mesh.vertices[v].normal) - expected));
            }
        }
    // The outer map edge is one-sided, the inner edges central.
    CHECK(worst <= 1e-6);

    SECTION("a missing edge neighbour is reported, a missing diagonal one is not needed")
    {
        Tiles partial(layout, Hills);
        partial.Erase({0, -1});

        TerrainMeshResult left = BuildTerrainTileMesh(layout, MeshRelease(), {-1, -1}, partial.Lookup());
        CHECK(left.status == TerrainMeshStatus::MissingNeighbour);
        CHECK(left.tile == TileCoord{0, -1});
        CHECK_FALSE(left.mesh);

        TerrainMeshResult above = BuildTerrainTileMesh(layout, MeshRelease(), {0, 0}, partial.Lookup());
        CHECK(above.status == TerrainMeshStatus::MissingNeighbour);
        CHECK(above.tile == TileCoord{0, -1});

        CHECK(BuildTerrainTileMesh(layout, MeshRelease(), {-1, 0}, partial.Lookup()).IsOk());
    }

    SECTION("a lone tile needs no neighbour")
    {
        const auto single = TerrainLayout::Create(4.0, 64, {0.0, 0.0, 256.0, 256.0});
        REQUIRE(single);
        const Tiles one(*single, Hills);
        const TerrainTileMesh mesh = BuildOk(*single, {0, 0}, one.Lookup());
        double loneWorst = 0.0;
        for (size_t v = 0; v < mesh.vertices.size(); ++v)
            loneWorst = std::max(loneWorst, glm::length(glm::dvec3(mesh.vertices[v].normal) -
                                                        ExpectedNormal(*single, one, SampleOf(mesh, v))));
        CHECK(loneWorst <= 1e-6);
    }
}

TEST_CASE("Tile mesh bounds are the extremes of its vertices", "[terrain][mesh]")
{
    const TerrainLayout layout = Block({-600, 585});
    const Tiles tiles(layout, Hills);
    const TerrainTileMesh mesh = BuildOk(layout, {-599, 585}, tiles.Lookup());

    float lowest = std::numeric_limits<float>::infinity();
    float highest = -lowest;
    for (const TerrainMeshVertex &v : mesh.vertices)
    {
        lowest = std::min(lowest, v.position.y);
        highest = std::max(highest, v.position.y);
    }
    CHECK(mesh.boundsMin == glm::vec3(0.0f, lowest, 0.0f));
    CHECK(mesh.boundsMax == glm::vec3(256.0f, highest, 256.0f));
    CHECK(lowest < highest);
}

TEST_CASE("A tile clipped by the map bounds meshes the cells that touch the map", "[terrain][mesh]")
{
    const auto layout = TerrainLayout::Create(4.0, 64, {-300.5, -130.25, 201.5, 260.75});
    REQUIRE(layout);
    const Tiles tiles(*layout, Hills);
    const TileSampleLookup lookup = tiles.Lookup();
    const TileRange range = layout->Tiles();
    REQUIRE(range.min == TileCoord{-2, -1});
    REQUIRE(range.max == TileCoord{0, 1});

    std::unordered_map<TileCoord, TerrainTileMesh> meshes;
    for (int32_t tz = range.min.z; tz <= range.max.z; ++tz)
        for (int32_t tx = range.min.x; tx <= range.max.x; ++tx)
        {
            const TileCoord tile{tx, tz};
            INFO("tile " << tx << ", " << tz);
            TerrainTileMesh mesh = BuildOk(*layout, tile, lookup);
            const TerrainLocalBounds extent = *layout->ValidExtent(tile);

            CHECK(layout->SamplePosition(mesh.samples.min.x) <= extent.minX);
            CHECK(extent.minX < layout->SamplePosition(mesh.samples.min.x + 1));
            CHECK(layout->SamplePosition(mesh.samples.max.x - 1) < extent.maxX);
            CHECK(extent.maxX <= layout->SamplePosition(mesh.samples.max.x));
            CHECK(layout->SamplePosition(mesh.samples.min.z) <= extent.minZ);
            CHECK(extent.minZ < layout->SamplePosition(mesh.samples.min.z + 1));
            CHECK(layout->SamplePosition(mesh.samples.max.z - 1) < extent.maxZ);
            CHECK(extent.maxZ <= layout->SamplePosition(mesh.samples.max.z));

            const int64_t rows = mesh.samples.max.z - mesh.samples.min.z + 1;
            CHECK(mesh.vertices.size() == size_t(Columns(mesh) * rows));
            CHECK(mesh.indices.size() == size_t((Columns(mesh) - 1) * (rows - 1) * 6));
            CHECK(mesh.origin.Meters().x == layout->SamplePosition(layout->TileFirstSample(tx)));
            int padding = 0;
            for (size_t v = 0; v < mesh.vertices.size(); ++v)
            {
                const GlobalSampleIndex sample = SampleOf(mesh, v);
                padding += layout->IsPaddingSample(sample) ? 1 : 0;
                CHECK(mesh.vertices[v].position.y == tiles.At(tile, sample));
            }
            CHECK((padding > 0) == (tx != -1 || tz != 0));
            meshes.emplace(tile, std::move(mesh));
        }

    std::mt19937_64 rng(0x14c);
    const TerrainLocalBounds &b = layout->Bounds();
    std::uniform_real_distribution<double> xs(b.minX, b.maxX), zs(b.minZ, b.maxZ);
    int failures = 0;
    for (int probe = 0; probe < 4000; ++probe)
    {
        const double x = probe == 0 ? b.maxX : xs(rng);
        const double z = probe == 0 ? b.maxZ : zs(rng);
        const SurfaceSample s = EvaluateSurface(*layout, Local(x, z), lookup);
        const TerrainTileMesh &mesh = meshes.at(s.tile);
        const bool inMesh = s.cell.x >= mesh.samples.min.x && s.cell.x < mesh.samples.max.x &&
                            s.cell.z >= mesh.samples.min.z && s.cell.z < mesh.samples.max.z;
        if (!s.IsOk() || !inMesh)
        {
            ++failures;
            continue;
        }
        const int64_t cell = (s.cell.z - mesh.samples.min.z) * (Columns(mesh) - 1) + (s.cell.x - mesh.samples.min.x);
        const std::array<glm::dvec3, 3> c = TriangleOf(mesh, size_t(cell * 2 + s.triangle));
        const glm::dvec3 n = glm::cross(c[1] - c[0], c[2] - c[0]);
        const double planeHeight = c[0].y - (n.x * (x - c[0].x) + n.z * (z - c[0].z)) / n.y;
        if (std::abs(planeHeight - s.height) > SurfaceTolerance(tiles.MaxAbsHeight()))
            ++failures;
    }
    CHECK(failures == 0);
}

TEST_CASE("Missing or bad samples are a status, never a mesh", "[terrain][mesh]")
{
    const TerrainLayout layout = Block({-1, -1});
    const TileCoord tile{-1, -1};

    CHECK(BuildTerrainTileMesh(layout, MeshRelease(), {5, 5}, Tiles(layout, Hills).Lookup()).status ==
          TerrainMeshStatus::InvalidInput);
    CHECK(BuildTerrainTileMesh(layout, MeshRelease(), tile, TileSampleLookup{}).status == TerrainMeshStatus::InvalidInput);

    Tiles missing(layout, Hills);
    missing.Erase(tile);
    TerrainMeshResult result = BuildTerrainTileMesh(layout, MeshRelease(), tile, missing.Lookup());
    CHECK(result.status == TerrainMeshStatus::MissingTile);
    CHECK(result.tile == tile);
    CHECK_FALSE(result.mesh);

    const std::vector<float> shortBlock(64 * 64, 1.0f);
    result = BuildTerrainTileMesh(layout, MeshRelease(), tile,
                                  [&](TileCoord) { return std::span<const float>(shortBlock); });
    CHECK(result.status == TerrainMeshStatus::InvalidSamples);

    Tiles bad(layout, Hills);
    bad.Set(tile, {-20, -7}, kNaNf);
    result = BuildTerrainTileMesh(layout, MeshRelease(), tile, bad.Lookup());
    CHECK(result.status == TerrainMeshStatus::InvalidSamples);
    CHECK(result.tile == tile);
    CHECK_FALSE(result.mesh);

    // Only the neighbour's first column feeds edge normals.
    Tiles badNeighbour(layout, Hills);
    badNeighbour.Set({0, -1}, {1, -30}, kNaNf);
    result = BuildTerrainTileMesh(layout, MeshRelease(), tile, badNeighbour.Lookup());
    CHECK(result.status == TerrainMeshStatus::InvalidSamples);
    CHECK(result.tile == TileCoord{0, -1});

    Tiles unread(layout, Hills);
    unread.Set({0, -1}, {5, -30}, kNaNf);
    CHECK(BuildTerrainTileMesh(layout, MeshRelease(), tile, unread.Lookup()).IsOk());
}

TEST_CASE("A tile mesh carries the identity of the release it was read from", "[terrain][mesh]")
{
    const AssetId asset = test::TerrainAsset("terrain/mesh_release.terrain.json");
    const TerrainLayout layout = test::EdgeLayout();
    const std::vector<TileHeights> baked = test::BuildTiles(layout);
    const test::OpenedRelease opened = test::OpenInMemory(test::Encode(test::Info(asset, 0x1400c0de), layout, baked));

    std::unordered_map<TileCoord, TileHeights> resident;
    const TileRange range = layout.Tiles();
    for (int32_t tz = range.min.z; tz <= range.max.z; ++tz)
        for (int32_t tx = range.min.x; tx <= range.max.x; ++tx)
            resident.emplace(TileCoord{tx, tz}, test::ReadOk(opened.handle, {tx, tz}));
    const TileSampleLookup lookup = [&](TileCoord tile)
    {
        const auto it = resident.find(tile);
        return it == resident.end() ? std::span<const float>{} : it->second.Samples();
    };

    const TileCoord tile{-1, 0};
    TerrainMeshResult built = BuildTerrainTileMesh(opened.handle.Layout(), opened.handle.Info(), tile, lookup);
    REQUIRE(built.IsOk());
    const TerrainTileMesh &mesh = *built.mesh;
    CHECK(mesh.release == opened.handle.Info());
    CHECK(mesh.release.release == TerrainReleaseId(0x1400c0de));
    for (size_t v = 0; v < mesh.vertices.size(); ++v)
    {
        const GlobalSampleIndex sample = SampleOf(mesh, v);
        CHECK(mesh.vertices[v].position.y == test::AnalyticHeight(sample.x, sample.z));
    }

    const TerrainPlacement placement{asset, At({1000.25, -40.0, -2000.5})};
    const std::optional<WorldPosition> origin = TerrainTileMeshWorldOrigin(mesh, placement);
    REQUIRE(origin);
    CHECK(glm::dvec3(origin->Meters()) == glm::dvec3(1000.25 - 256.0, -40.0, -2000.5));

    const TerrainPlacement other{test::TerrainAsset("terrain/other.terrain.json"), placement.translation};
    CHECK_FALSE(TerrainTileMeshWorldOrigin(mesh, other));
}

TEST_CASE("Far tile meshes keep millimetre vertices after camera-relative narrowing", "[terrain][mesh][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    struct Case
    {
        TileCoord min;
        glm::dvec3 placement;
    };
    const Case cases[]{{{-1, -1}, {-150000.125, 350.5, 75000.5}}, {{-600, 585}, {0.25, -20.0, -0.75}}};
    const glm::dvec3 kStep{0.001, -0.002, 0.003};

    for (const Case &c : cases)
    {
        INFO("block " << c.min.x << ", " << c.min.z);
        const TerrainLayout layout = Block(c.min);
        const Tiles tiles(layout, Hills);
        const TerrainPlacement placement{MeshRelease().asset, At(c.placement)};

        struct Placed
        {
            TerrainTileMesh mesh;
            WorldPose pose;
        };
        std::unordered_map<TileCoord, Placed> placed;
        for (int32_t dz = 0; dz <= 1; ++dz)
            for (int32_t dx = 0; dx <= 1; ++dx)
            {
                TerrainTileMesh mesh = BuildOk(layout, {c.min.x + dx, c.min.z + dz}, tiles.Lookup());
                const std::optional<WorldPosition> origin = TerrainTileMeshWorldOrigin(mesh, placement);
                REQUIRE(origin);
                placed.emplace(mesh.tile, Placed{std::move(mesh), WorldPose{*origin}});
            }

        const GlobalSampleIndex corner{layout.TileFirstSample(c.min.x + 1), layout.TileFirstSample(c.min.z + 1)};
        const TerrainTileMesh &upper = placed.at({c.min.x + 1, c.min.z + 1}).mesh;
        const glm::dvec3 cornerWorld = c.placement + LocalOf(upper, VertexAt(upper, corner));
        // None sits on float's 1/64 m grid out there.
        const glm::dvec3 cameras[]{cornerWorld + glm::dvec3(0.37, 1.7, -0.21), cornerWorld + kStep,
                                   cornerWorld + glm::dvec3(-300.3, 150.0, 210.7)};

        for (const glm::dvec3 &camera : cameras)
        {
            double worst = 0.0;
            double worstNaive = 0.0;
            double farthest = 0.0;
            std::unordered_map<TileCoord, glm::mat4> models;
            for (const auto &[tile, p] : placed)
            {
                const std::optional<glm::mat4> model = p.pose.TryMatrixRelativeTo(At(camera));
                REQUIRE(model);
                models.emplace(tile, *model);
                for (const TerrainMeshVertex &v : p.mesh.vertices)
                {
                    const glm::dvec3 render = glm::dvec3(glm::vec3(*model * glm::vec4(v.position, 1.0f)));
                    const glm::dvec3 world = c.placement + LocalOf(p.mesh, v);
                    const glm::dvec3 exact = world - camera;
                    const glm::dvec3 naive =
                        glm::dvec3(glm::vec3(world) - glm::vec3(camera)); // absolute positions narrowed first
                    worst = std::max(worst, glm::length(render - exact));
                    worstNaive = std::max(worstNaive, glm::length(naive - exact));
                    farthest = std::max(farthest, glm::length(exact));
                }
            }
            INFO("camera offset " << (camera - cornerWorld).x << ", " << (camera - cornerWorld).y);
            const double ulp = std::ldexp(1.0, std::ilogb(farthest) - 23);
            CHECK(worst <= ulp);
            CHECK(worstNaive > 1e-3);

            // Equal across a shared edge, at most one ulp along it.
            double edgeGap = 0.0;
            for (int64_t s = 0; s <= 64; ++s)
            {
                const GlobalSampleIndex sample{corner.x, layout.TileFirstSample(c.min.z) + s};
                const TileCoord a{c.min.x, c.min.z}, b{c.min.x + 1, c.min.z};
                const glm::vec4 pa = models.at(a) * glm::vec4(VertexAt(placed.at(a).mesh, sample).position, 1.0f);
                const glm::vec4 pb = models.at(b) * glm::vec4(VertexAt(placed.at(b).mesh, sample).position, 1.0f);
                CHECK(pa.y == pb.y);
                CHECK(pa.z == pb.z);
                edgeGap = std::max(edgeGap, double(std::abs(pa.x - pb.x)));
            }
            CHECK(edgeGap <= ulp);
        }
    }
}

TEST_CASE("Tile mesh sizes for 65 and 257 samples a side", "[terrain][mesh]")
{
    CHECK(sizeof(TerrainMeshVertex) == 24);

    struct Size
    {
        double spacing;
        int32_t n;
    };
    for (const Size size : {Size{4.0, 64}, Size{1.0, 256}})
    {
        const auto layout = TerrainLayout::Create(size.spacing, size.n, {0.0, 0.0, 256.0, 256.0});
        REQUIRE(layout);
        const Tiles tiles(*layout, Hills);
        const TerrainTileMesh mesh = BuildOk(*layout, {0, 0}, tiles.Lookup());
        const size_t side = size_t(size.n) + 1;
        CHECK(mesh.vertices.size() == side * side);
        CHECK(mesh.indices.size() == size_t(size.n) * size_t(size.n) * 6);
        CHECK(mesh.boundsMax.x == 256.0f);
        CHECK(mesh.boundsMax.z == 256.0f);
    }
}
