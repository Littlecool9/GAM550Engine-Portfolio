/**
 * @file TerrainSurface.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainSurface.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

using namespace mir;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();
    constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

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

    // Heights come from global positions, so shared edge samples are equal.
    class TileSet
    {
    public:
        TileSet(const TerrainLayout &layout, double (*height)(double, double)) : mLayout(layout)
        {
            const TileRange range = layout.Tiles();
            const int64_t side = Side();
            for (int32_t tz = range.min.z; tz <= range.max.z; ++tz)
                for (int32_t tx = range.min.x; tx <= range.max.x; ++tx)
                {
                    std::vector<float> &block = mTiles[TileCoord{tx, tz}];
                    block.resize(static_cast<size_t>(side * side));
                    const int64_t i0 = layout.TileFirstSample(tx);
                    const int64_t k0 = layout.TileFirstSample(tz);
                    for (int64_t k = 0; k < side; ++k)
                        for (int64_t i = 0; i < side; ++i)
                            block[static_cast<size_t>(k * side + i)] = static_cast<float>(
                                height(layout.SamplePosition(i0 + i), layout.SamplePosition(k0 + k)));
                }
        }

        std::span<const float> Samples(TileCoord tile) const
        {
            const auto it = mTiles.find(tile);
            return it == mTiles.end() ? std::span<const float>{} : std::span<const float>(it->second);
        }

        TileSamplesView View(TileCoord tile) const { return {tile, Samples(tile)}; }

        TileSampleLookup Lookup() const
        {
            return [this](TileCoord tile) { return Samples(tile); };
        }

        float At(TileCoord tile, GlobalSampleIndex sample) const { return mTiles.at(tile)[Offset(tile, sample)]; }
        void Set(TileCoord tile, GlobalSampleIndex sample, float value) { mTiles.at(tile)[Offset(tile, sample)] = value; }
        void Erase(TileCoord tile) { mTiles.erase(tile); }

        double MaxAbsHeight() const
        {
            double result = 0.0;
            for (const auto &[tile, block] : mTiles)
                for (const float h : block)
                    result = std::max(result, std::abs(double(h)));
            return result;
        }

    private:
        int64_t Side() const { return int64_t(mLayout.IntervalsPerTile()) + 1; }

        size_t Offset(TileCoord tile, GlobalSampleIndex sample) const
        {
            const int64_t i = sample.x - mLayout.TileFirstSample(tile.x);
            const int64_t k = sample.z - mLayout.TileFirstSample(tile.z);
            return static_cast<size_t>(k * Side() + i);
        }

        TerrainLayout mLayout;
        std::unordered_map<TileCoord, std::vector<float>> mTiles;
    };

    TileCoord TileOfCell(const TerrainLayout &layout, GlobalSampleIndex cell)
    {
        return {layout.TileOfCell(cell.x), layout.TileOfCell(cell.z)};
    }

    std::array<glm::dvec3, 3> MeshTriangle(const TerrainLayout &layout, const TileSet &tiles, GlobalSampleIndex cell,
                                           uint8_t triangle)
    {
        const TileCoord tile = TileOfCell(layout, cell);
        const std::array<GlobalSampleIndex, 3> &corners = TriangulateCell(cell).triangles[triangle];
        std::array<glm::dvec3, 3> vertices{};
        for (size_t v = 0; v < 3; ++v)
            vertices[v] = {layout.SamplePosition(corners[v].x), tiles.At(tile, corners[v]),
                           layout.SamplePosition(corners[v].z)};
        return vertices;
    }

    struct PlaneHit
    {
        double height = 0.0;
        // Below zero: the point is outside the triangle.
        double minWeight = 0.0;
    };

    // Deliberately not the evaluator's formula.
    PlaneHit PlaneAt(const std::array<glm::dvec3, 3> &v, double x, double z)
    {
        const double ax = v[1].x - v[0].x, az = v[1].z - v[0].z;
        const double bx = v[2].x - v[0].x, bz = v[2].z - v[0].z;
        const double px = x - v[0].x, pz = z - v[0].z;
        const double d = ax * bz - bx * az;
        const double w1 = (px * bz - bx * pz) / d;
        const double w2 = (ax * pz - px * az) / d;
        const double w0 = 1.0 - w1 - w2;
        return {w0 * v[0].y + w1 * v[1].y + w2 * v[2].y, std::min({w0, w1, w2})};
    }

    TerrainLayout Block(TileCoord min)
    {
        const double t = 256.0;
        const auto layout = TerrainLayout::Create(4.0, 64, {min.x * t, min.z * t, (min.x + 2) * t, (min.z + 2) * t});
        REQUIRE(layout);
        return *layout;
    }

    // About 150 km out a double ulp is 2.9e-11 m.
    constexpr TileCoord kBlocks[]{{-1, -1}, {-5, 2}, {-600, 585}};

    // Worst difference between the owner and every other cell holding the point.
    double WorstOtherSide(const TerrainLayout &layout, const TileSet &tiles, double x, double z,
                          const SurfaceSample &owner, int &sides)
    {
        double worst = 0.0;
        for (int64_t dz = -1; dz <= 0; ++dz)
            for (int64_t dx = -1; dx <= 0; ++dx)
            {
                const GlobalSampleIndex cell{owner.cell.x + dx, owner.cell.z + dz};
                const bool holds = layout.SamplePosition(cell.x) <= x && x <= layout.SamplePosition(cell.x + 1) &&
                                   layout.SamplePosition(cell.z) <= z && z <= layout.SamplePosition(cell.z + 1);
                const TileCoord tile = TileOfCell(layout, cell);
                if (!holds || !layout.Contains(tile))
                    continue;
                const SurfaceSample side = EvaluateSurfaceInCell(layout, tiles.View(tile), cell, Local(x, z));
                if (!side.IsOk())
                    return kInf;
                worst = std::max(worst, std::abs(side.height - owner.height));
                ++sides;
            }
        return worst;
    }
}

TEST_CASE("Every cell is cut along the same diagonal, counter-clockwise from above", "[terrain][surface]")
{
    const GlobalSampleIndex cells[]{{0, 0}, {-1, -1}, {63, 64}, {64, 63}, {-65, 17}, {-38401, 37440}};
    for (const GlobalSampleIndex cell : cells)
    {
        INFO("cell " << cell.x << ", " << cell.z);
        const GlobalSampleIndex c10{cell.x + 1, cell.z};
        const GlobalSampleIndex c01{cell.x, cell.z + 1};
        const GlobalSampleIndex c11{cell.x + 1, cell.z + 1};

        const CellTriangulation split = TriangulateCell(cell);
        CHECK(split.diagonal[0] == cell);
        CHECK(split.diagonal[1] == c11);
        CHECK(split.triangles[0] == std::array<GlobalSampleIndex, 3>{cell, c01, c11});
        CHECK(split.triangles[1] == std::array<GlobalSampleIndex, 3>{cell, c11, c10});

        for (const std::array<GlobalSampleIndex, 3> &tri : split.triangles)
        {
            // y of cross(b - a, c - a): positive means counter-clockwise seen from +Y.
            const int64_t abx = tri[1].x - tri[0].x, abz = tri[1].z - tri[0].z;
            const int64_t acx = tri[2].x - tri[0].x, acz = tri[2].z - tri[0].z;
            CHECK(abz * acx - abx * acz > 0);
            CHECK(std::count(tri.begin(), tri.end(), split.diagonal[0]) == 1);
            CHECK(std::count(tri.begin(), tri.end(), split.diagonal[1]) == 1);
        }

        const GlobalSampleIndex shifts[]{{1, 0}, {0, 1}, {64, -64}, {-1, 3}};
        for (const GlobalSampleIndex shift : shifts)
        {
            const CellTriangulation moved = TriangulateCell({cell.x + shift.x, cell.z + shift.z});
            for (size_t t = 0; t < 2; ++t)
                for (size_t v = 0; v < 3; ++v)
                    CHECK(moved.triangles[t][v] == GlobalSampleIndex{split.triangles[t][v].x + shift.x,
                                                                     split.triangles[t][v].z + shift.z});
        }
    }
}

TEST_CASE("The surface passes through every stored sample", "[terrain][surface]")
{
    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const TileSet tiles(layout, Hills);
        const TileSampleLookup lookup = tiles.Lookup();

        int mismatches = 0;
        for (int64_t k = layout.TileFirstSample(min.z); k <= layout.TileFirstSample(min.z + 2); ++k)
            for (int64_t i = layout.TileFirstSample(min.x); i <= layout.TileFirstSample(min.x + 2); ++i)
            {
                // The closed max edge stays in the last tile.
                const TileCoord owner{std::min(layout.TileOfCell(i), min.x + 1),
                                      std::min(layout.TileOfCell(k), min.z + 1)};
                const SurfaceSample s =
                    EvaluateSurface(layout, Local(layout.SamplePosition(i), layout.SamplePosition(k)), lookup);
                if (!s.IsOk() || s.tile != owner || s.height != double(tiles.At(owner, {i, k})))
                    ++mismatches;
            }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Shared tile edges evaluate to one height from every side", "[terrain][surface]")
{
    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const TileSet tiles(layout, Hills);
        const TileSampleLookup lookup = tiles.Lookup();
        const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());
        const TerrainLocalBounds &bounds = layout.Bounds();
        const double edgeX = layout.SamplePosition(layout.TileFirstSample(min.x + 1));
        const double edgeZ = layout.SamplePosition(layout.TileFirstSample(min.z + 1));

        double onEdge = 0.0;
        double acrossEdge = 0.0;
        int sides = 0;
        int failures = 0;
        for (int step = 0; step <= 2048; ++step)
            for (const bool vertical : {true, false})
            {
                const double x = vertical ? edgeX : bounds.minX + 0.25 * step;
                const double z = vertical ? bounds.minZ + 0.25 * step : edgeZ;
                const SurfaceSample owner = EvaluateSurface(layout, Local(x, z), lookup);
                const int32_t ownerAxis = vertical ? owner.tile.x : owner.tile.z;
                const int32_t upperTile = vertical ? min.x + 1 : min.z + 1;
                if (!owner.IsOk() || ownerAxis != upperTile)
                {
                    ++failures;
                    continue;
                }
                onEdge = std::max(onEdge, WorstOtherSide(layout, tiles, x, z, owner, sides));

                for (const double direction : {-kInf, kInf})
                {
                    const SurfaceSample near =
                        vertical ? EvaluateSurface(layout, Local(std::nextafter(x, direction), z), lookup)
                                 : EvaluateSurface(layout, Local(x, std::nextafter(z, direction)), lookup);
                    const int32_t nearAxis = vertical ? near.tile.x : near.tile.z;
                    if (!near.IsOk() || nearAxis != (direction < 0.0 ? upperTile - 1 : upperTile))
                        ++failures;
                    else
                        acrossEdge = std::max(acrossEdge, std::abs(near.height - owner.height));
                }
            }

        CHECK(failures == 0);
        CHECK(sides > 2 * 2 * 2049);
        CHECK(onEdge == 0.0);
        CHECK(acrossEdge <= tolerance);
    }
}

TEST_CASE("A four-tile corner evaluates to its shared sample from all four tiles", "[terrain][surface]")
{
    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const TileSet tiles(layout, Hills);
        const TileSampleLookup lookup = tiles.Lookup();
        const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());

        const GlobalSampleIndex corner{layout.TileFirstSample(min.x + 1), layout.TileFirstSample(min.z + 1)};
        const double x = layout.SamplePosition(corner.x);
        const double z = layout.SamplePosition(corner.z);
        const TileCoord upper{min.x + 1, min.z + 1};
        const double stored = tiles.At(upper, corner);

        const SurfaceSample owner = EvaluateSurface(layout, Local(x, z), lookup);
        REQUIRE(owner.IsOk());
        CHECK(owner.tile == upper);
        CHECK(owner.cell == corner);
        CHECK(owner.height == stored);

        for (int32_t dz = 0; dz <= 1; ++dz)
            for (int32_t dx = 0; dx <= 1; ++dx)
            {
                const TileCoord tile{min.x + dx, min.z + dz};
                INFO("tile " << tile.x << ", " << tile.z);
                CHECK(double(tiles.At(tile, corner)) == stored);

                const GlobalSampleIndex cell{corner.x - 1 + dx, corner.z - 1 + dz};
                const SurfaceSample side = EvaluateSurfaceInCell(layout, tiles.View(tile), cell, Local(x, z));
                REQUIRE(side.IsOk());
                CHECK(side.height == stored);

                const double nearX = std::nextafter(x, dx == 1 ? kInf : -kInf);
                const double nearZ = std::nextafter(z, dz == 1 ? kInf : -kInf);
                const SurfaceSample near = EvaluateSurface(layout, Local(nearX, nearZ), lookup);
                REQUIRE(near.IsOk());
                CHECK(near.tile == tile);
                CHECK(std::abs(near.height - stored) <= tolerance);
            }
    }
}

TEST_CASE("Random points match planes through the mesh triangles of the same samples", "[terrain][surface]")
{
    std::mt19937_64 random(0x114);
    // 53 raw bits: the same on every standard library.
    const auto unit = [&random] { return double(random() >> 11) * 0x1.0p-53; };

    for (const TileCoord min : kBlocks)
    {
        INFO("block " << min.x << ", " << min.z);
        const TerrainLayout layout = Block(min);
        const TileSet tiles(layout, Hills);
        const TileSampleLookup lookup = tiles.Lookup();
        const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());
        const int64_t firstCell = layout.TileFirstSample(min.x);
        const int64_t firstRow = layout.TileFirstSample(min.z);
        const int64_t cells = 2 * int64_t(layout.IntervalsPerTile());

        double worstHeight = 0.0;
        double worstNormal = 0.0;
        int failures = 0;
        for (int n = 0; n < 20000; ++n)
        {
            const int64_t i = firstCell + std::min(int64_t(unit() * double(cells)), cells - 1);
            const int64_t k = firstRow + std::min(int64_t(unit() * double(cells)), cells - 1);
            double fx = unit();
            double fz = unit();
            // A quarter on the cell diagonal.
            if (n % 4 == 1)
                fz = fx;
            else if (n % 8 == 2)
                fx = 0.0;
            else if (n % 8 == 6)
                fz = 0.0;
            const double x = layout.SamplePosition(i) + fx * layout.SampleSpacing();
            const double z = layout.SamplePosition(k) + fz * layout.SampleSpacing();

            const SurfaceSample s = EvaluateSurface(layout, Local(x, z), lookup);
            if (!s.IsOk())
            {
                ++failures;
                continue;
            }
            const std::array<glm::dvec3, 3> triangle = MeshTriangle(layout, tiles, s.cell, s.triangle);
            const PlaneHit plane = PlaneAt(triangle, x, z);
            if (plane.minWeight < -1e-12)
                ++failures;
            worstHeight = std::max(worstHeight, std::abs(plane.height - s.height));

            const glm::dvec3 up = glm::normalize(glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]));
            worstNormal = std::max(worstNormal, glm::length(up - s.normal));
        }

        CHECK(failures == 0);
        CHECK(worstHeight <= tolerance);
        CHECK(worstNormal <= 1e-12);
    }
}

TEST_CASE("The closed max edge evaluates in the last cell", "[terrain][surface]")
{
    const TerrainLayout layout = Block({-1, -1});
    const TileSet tiles(layout, Hills);
    const TileSampleLookup lookup = tiles.Lookup();
    const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());

    for (const double along : {-256.0, -100.5, -4.0, 0.0, 3.25, 255.0})
    {
        INFO("along " << along);
        for (const bool onX : {true, false})
        {
            const double x = onX ? 256.0 : along;
            const double z = onX ? along : 256.0;
            const SurfaceSample edge = EvaluateSurface(layout, Local(x, z), lookup);
            REQUIRE(edge.IsOk());
            CHECK((onX ? edge.tile.x : edge.tile.z) == 0);
            CHECK((onX ? edge.cell.x : edge.cell.z) == 63);

            const PlaneHit plane = PlaneAt(MeshTriangle(layout, tiles, edge.cell, edge.triangle), x, z);
            CHECK(plane.minWeight >= -1e-12);
            CHECK(std::abs(plane.height - edge.height) <= tolerance);

            const SurfaceSample past = onX ? EvaluateSurface(layout, Local(std::nextafter(x, kInf), z), lookup)
                                           : EvaluateSurface(layout, Local(x, std::nextafter(z, kInf)), lookup);
            CHECK(past.status == SurfaceStatus::OutsideMap);
            CHECK(std::isnan(past.height));
        }
    }

    const SurfaceSample corner = EvaluateSurface(layout, Local(256.0, 256.0), lookup);
    REQUIRE(corner.IsOk());
    CHECK(corner.tile == TileCoord{0, 0});
    CHECK(corner.cell == GlobalSampleIndex{63, 63});
    CHECK(corner.height == double(tiles.At({0, 0}, {64, 64})));
}

TEST_CASE("A partial boundary tile reads its padding samples and stops at the bounds", "[terrain][surface]")
{
    const auto created = TerrainLayout::Create(4.0, 64, {-300.5, -130.25, 201.5, 260.75});
    REQUIRE(created);
    const TerrainLayout &layout = *created;
    TileSet tiles(layout, Hills);
    const TileSampleLookup lookup = tiles.Lookup();
    const double tolerance = SurfaceTolerance(tiles.MaxAbsHeight());
    REQUIRE(layout.Tiles().min == TileCoord{-2, -1});
    REQUIRE(layout.Tiles().max == TileCoord{0, 1});

    struct Probe
    {
        double x, z;
        // The cell's sample beyond the bounds.
        GlobalSampleIndex padding;
        double outX, outZ;
    };
    const Probe probes[]{
        {201.5, 10.0, {51, 2}, std::nextafter(201.5, kInf), 10.0},
        {-300.5, 10.0, {-76, 2}, std::nextafter(-300.5, -kInf), 10.0},
        {0.5, 260.75, {0, 66}, 0.5, std::nextafter(260.75, kInf)},
        {0.5, -130.25, {0, -33}, 0.5, std::nextafter(-130.25, -kInf)},
        {201.5, 260.75, {51, 66}, std::nextafter(201.5, kInf), std::nextafter(260.75, kInf)},
    };
    for (const Probe &p : probes)
    {
        INFO("probe " << p.x << ", " << p.z);
        CHECK(layout.IsPaddingSample(p.padding));

        const SurfaceSample s = EvaluateSurface(layout, Local(p.x, p.z), lookup);
        REQUIRE(s.IsOk());
        const PlaneHit plane = PlaneAt(MeshTriangle(layout, tiles, s.cell, s.triangle), p.x, p.z);
        CHECK(plane.minWeight >= -1e-12);
        CHECK(std::abs(plane.height - s.height) <= tolerance);

        CHECK(EvaluateSurface(layout, Local(p.outX, p.outZ), lookup).status == SurfaceStatus::OutsideMap);
        CHECK(EvaluateSurfaceInCell(layout, tiles.View(s.tile), s.cell, Local(p.outX, p.outZ)).status ==
              SurfaceStatus::OutsideMap);
    }

    tiles.Set({0, 0}, {51, 2}, kNaNf);
    const SurfaceSample touching = EvaluateSurface(layout, Local(201.5, 10.0), lookup);
    CHECK(touching.status == SurfaceStatus::InvalidSamples);
    CHECK(touching.tile == TileCoord{0, 0});
    CHECK(std::isnan(touching.height));
    CHECK(EvaluateSurface(layout, Local(201.5, 6.0), lookup).status == SurfaceStatus::InvalidSamples);
    CHECK(EvaluateSurface(layout, Local(199.0, 10.0), lookup).IsOk());
    CHECK(EvaluateSurface(layout, Local(201.5, 13.0), lookup).IsOk());
}

TEST_CASE("Out-of-map, missing or invalid samples never yield a height", "[terrain][surface]")
{
    const TerrainLayout layout = Block({-1, -1});
    TileSet tiles(layout, Hills);

    const SurfaceSample outside = EvaluateSurface(layout, Local(300.0, 0.0), tiles.Lookup());
    CHECK(outside.status == SurfaceStatus::OutsideMap);
    CHECK(std::isnan(outside.height));
    CHECK(std::isnan(outside.normal.y));

    CHECK(EvaluateSurface(layout, Local(10.0, 10.0), TileSampleLookup{}).status == SurfaceStatus::InvalidInput);

    // The neighbour's copy of a shared edge is not a fallback.
    TileSet partial = tiles;
    partial.Erase({0, 0});
    const TileSampleLookup partialLookup = partial.Lookup();
    const SurfaceSample missing = EvaluateSurface(layout, Local(0.0, 100.0), partialLookup);
    CHECK(missing.status == SurfaceStatus::MissingTile);
    CHECK(missing.tile == TileCoord{0, 0});
    CHECK(std::isnan(missing.height));
    CHECK(EvaluateSurface(layout, Local(std::nextafter(0.0, -kInf), 100.0), partialLookup).IsOk());

    const std::vector<float> shortBlock(64 * 64, 100.0f);
    const TileSampleLookup shortLookup = [&shortBlock](TileCoord) { return std::span<const float>(shortBlock); };
    CHECK(EvaluateSurface(layout, Local(10.0, 10.0), shortLookup).status == SurfaceStatus::InvalidSamples);

    // Sample (10, 10) is at (40, 40).
    tiles.Set({0, 0}, {10, 10}, std::numeric_limits<float>::infinity());
    const TileSampleLookup lookup = tiles.Lookup();
    for (const double x : {39.0, 41.0})
        for (const double z : {39.0, 41.0})
            CHECK(EvaluateSurface(layout, Local(x, z), lookup).status == SurfaceStatus::InvalidSamples);
    CHECK(EvaluateSurface(layout, Local(35.0, 35.0), lookup).IsOk());
    CHECK(EvaluateSurface(layout, Local(45.0, 45.0), lookup).IsOk());

    const TileSamplesView view = tiles.View({0, 0});
    CHECK(EvaluateSurfaceInCell(layout, view, {3, 3}, Local(13.0, 13.0)).IsOk());
    CHECK(EvaluateSurfaceInCell(layout, view, {-1, 3}, Local(-2.0, 13.0)).status == SurfaceStatus::InvalidInput);
    CHECK(EvaluateSurfaceInCell(layout, view, {3, 3}, Local(20.0, 13.0)).status == SurfaceStatus::InvalidInput);
    CHECK(EvaluateSurfaceInCell(layout, TileSamplesView{{2, 0}, view.samples}, {130, 3}, Local(13.0, 13.0))
              .status == SurfaceStatus::InvalidInput);
    CHECK(EvaluateSurfaceInCell(layout, TileSamplesView{{0, 0}, {}}, {3, 3}, Local(13.0, 13.0)).status ==
          SurfaceStatus::MissingTile);
}

TEST_CASE("SurfaceTolerance is half a float ulp of the heights involved", "[terrain][surface]")
{
    CHECK(SurfaceTolerance(0.0) == 0x1.0p-24);
    CHECK(SurfaceTolerance(1.0) == 0x1.0p-24);
    CHECK(SurfaceTolerance(1.9) == 0x1.0p-24);
    CHECK(SurfaceTolerance(-300.0) == 0x1.0p-16);
    CHECK(SurfaceTolerance(8848.0) == 0x1.0p-11);

    // Clamped to the largest float's half ulp, never infinite.
    CHECK(SurfaceTolerance(std::numeric_limits<float>::max()) == 0x1.0p103);
    CHECK(SurfaceTolerance(1e300) == 0x1.0p103);
    CHECK(SurfaceTolerance(kInf) == 0x1.0p103);
    CHECK(SurfaceTolerance(-kInf) == 0x1.0p103);
    CHECK(SurfaceTolerance(std::numeric_limits<double>::quiet_NaN()) == 0x1.0p103);

    for (const float h : {1.0f, 229.5f, 300.0f, 8848.0f})
    {
        const float next = std::nextafter(h, std::numeric_limits<float>::infinity());
        CHECK(2.0 * SurfaceTolerance(h) == double(next) - double(h));
    }
}
