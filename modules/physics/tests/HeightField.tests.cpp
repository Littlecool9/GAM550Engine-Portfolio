/**
 * @file HeightField.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsWorld height fields.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>

#include <terrain/TerrainLayout.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

using namespace mir;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr double kSpacing = 4.0;
    constexpr int32_t kIntervals = 64; // 256 m tiles of 65 x 65 samples
    constexpr uint32_t kTileSide = kIntervals + 1;
    constexpr uint32_t kAreaSide = 2 * kIntervals + 1;

    // Jolt holds these fixtures within 3.24 cm (Hills) and 1.7 mm (Plane) of the surface.
    constexpr double kHillsQuantization = 0.035;
    constexpr double kPlaneQuantization = 0.0025;
    // Resting contacts may sink up to Jolt's 2 cm penetration slop.
    constexpr double kPenetration = 0.025;

    const glm::dvec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const glm::dvec3 kEdgeB{150000.625, -20.0, -75000.25};

    // About 5 m of twist in a 4 m cell, pits a few metres deep.
    double Hills(double x, double z)
    {
        return 180.0 + 40.0 * std::sin(0.011 * x + 0.7) * std::cos(0.017 * z - 0.4) +
               6.0 * std::sin(0.09 * x + 0.13 * z) + 3.0 * std::sin(0.37 * x) * std::sin(0.29 * z);
    }

    constexpr double kPlaneX = -0.08, kPlaneZ = -0.03;
    double Plane(double x, double z) { return 50.0 + kPlaneX * x + kPlaneZ * z; }

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    glm::dvec3 MetersOf(const PhysicsWorld &world, BodyHandle body)
    {
        const std::optional<WorldPosition> position = world.BodyPosition(body);
        REQUIRE(position);
        return glm::dvec3(position->Meters());
    }

    // Ericson, Real-Time Collision Detection 5.1.5: closest point on a triangle.
    glm::dvec3 ClosestOnTriangle(const glm::dvec3 &p, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c)
    {
        const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
        const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
        if (d1 <= 0.0 && d2 <= 0.0)
            return a;
        const glm::dvec3 bp = p - b;
        const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3)
            return b;
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
            return a + ab * (d1 / (d1 - d3));
        const glm::dvec3 cp = p - c;
        const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6)
            return c;
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
            return a + ac * (d2 / (d2 - d6));
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
            return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        const double denom = 1.0 / (va + vb + vc);
        return a + ab * (vb * denom) + ac * (vc * denom);
    }

    // A 2x2 release of 256 m tiles at 4 m, placed at `placement`.
    class Terrain
    {
    public:
        Terrain(double (*height)(double, double), const glm::dvec3 &placement)
            : mLayout(*TerrainLayout::CreateFromTileSize(kIntervals * kSpacing, kSpacing, {-256.0, -256.0, 256.0, 256.0}))
            , mPlacement(At(placement))
        {
            for (int32_t tz = -1; tz <= 0; ++tz)
                for (int32_t tx = -1; tx <= 0; ++tx)
                {
                    std::vector<float> &block = mTiles[TileCoord{tx, tz}];
                    block.resize(size_t(kTileSide) * kTileSide);
                    const int64_t i0 = mLayout.TileFirstSample(tx), k0 = mLayout.TileFirstSample(tz);
                    for (int64_t k = 0; k < kTileSide; ++k)
                        for (int64_t i = 0; i < kTileSide; ++i)
                            block[size_t(k * kTileSide + i)] = static_cast<float>(
                                height(mLayout.SamplePosition(i0 + i), mLayout.SamplePosition(k0 + k)));
                }
        }

        WorldPosition Placement() const { return mPlacement; }

        float Sample(GlobalSampleIndex s) const
        {
            // The closed max edge is in the last tile.
            const TileCoord tile{std::min(mLayout.TileOfCell(s.x), 0), std::min(mLayout.TileOfCell(s.z), 0)};
            const int64_t i = s.x - mLayout.TileFirstSample(tile.x), k = s.z - mLayout.TileFirstSample(tile.z);
            return mTiles.at(tile)[size_t(k * kTileSide + i)];
        }

        std::vector<float> Area() const
        {
            std::vector<float> area(size_t(kAreaSide) * kAreaSide);
            for (int64_t k = 0; k < kAreaSide; ++k)
                for (int64_t i = 0; i < kAreaSide; ++i)
                    area[size_t(k * kAreaSide + i)] = Sample({i - kIntervals, k - kIntervals});
            return area;
        }

        WorldPosition SampleOrigin(GlobalSampleIndex s) const
        {
            const auto local = TerrainLocalPosition::FromMeters({mLayout.SamplePosition(s.x), 0.0, mLayout.SamplePosition(s.z)});
            REQUIRE(local);
            const std::optional<WorldPosition> world = TryTerrainLocalToWorld(*local, mPlacement);
            REQUIRE(world);
            return *world;
        }

        // P1's policy: one shape over the whole area.
        BodyHandle AddArea(PhysicsWorld &world, const std::vector<float> &area, HeightFieldDesc desc = {}) const
        {
            desc.mSamples = area;
            desc.mCountX = kAreaSide;
            desc.mCountZ = kAreaSide;
            desc.mSpacing = static_cast<float>(kSpacing);
            desc.mOrigin = SampleOrigin({-kIntervals, -kIntervals});
            return world.CreateStaticHeightField(desc);
        }

        // The alternative: one shape per tile.
        std::vector<BodyHandle> AddTiles(PhysicsWorld &world) const
        {
            std::vector<BodyHandle> bodies;
            for (const auto &[tile, block] : mTiles)
            {
                HeightFieldDesc desc;
                desc.mSamples = block;
                desc.mCountX = kTileSide;
                desc.mCountZ = kTileSide;
                desc.mSpacing = static_cast<float>(kSpacing);
                desc.mOrigin = SampleOrigin({mLayout.TileFirstSample(tile.x), mLayout.TileFirstSample(tile.z)});
                bodies.push_back(world.CreateStaticHeightField(desc));
            }
            return bodies;
        }

        glm::dvec3 Local(const glm::dvec3 &world) const
        {
            const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(At(world), mPlacement);
            REQUIRE(local);
            return local->Meters();
        }

        glm::dvec3 World(const glm::dvec3 &local) const
        {
            const auto position = TerrainLocalPosition::FromMeters(local);
            REQUIRE(position);
            const std::optional<WorldPosition> world = TryTerrainLocalToWorld(*position, mPlacement);
            REQUIRE(world);
            return glm::dvec3(world->Meters());
        }

        SurfaceSample Surface(const glm::dvec3 &local) const
        {
            const auto position = TerrainLocalPosition::FromMeters(local);
            REQUIRE(position);
            const SurfaceSample s = EvaluateSurface(mLayout, *position, [this](TileCoord tile) {
                const auto it = mTiles.find(tile);
                return it == mTiles.end() ? std::span<const float>{} : std::span<const float>(it->second);
            });
            REQUIRE(s.IsOk());
            return s;
        }

        // Negative below the surface.
        double SignedDistance(const glm::dvec3 &local, double reach) const
        {
            const auto cell = [](double v) { return static_cast<int64_t>(std::floor(v / kSpacing)); };
            double best = std::numeric_limits<double>::infinity();
            for (int64_t k = std::max<int64_t>(cell(local.z - reach), -kIntervals);
                 k <= std::min<int64_t>(cell(local.z + reach), kIntervals - 1); ++k)
                for (int64_t i = std::max<int64_t>(cell(local.x - reach), -kIntervals);
                     i <= std::min<int64_t>(cell(local.x + reach), kIntervals - 1); ++i)
                    for (const std::array<GlobalSampleIndex, 3> &triangle : TriangulateCell({i, k}).triangles)
                    {
                        std::array<glm::dvec3, 3> v;
                        for (size_t j = 0; j < 3; ++j)
                            v[j] = {mLayout.SamplePosition(triangle[j].x), Sample(triangle[j]),
                                    mLayout.SamplePosition(triangle[j].z)};
                        best = std::min(best, glm::length(local - ClosestOnTriangle(local, v[0], v[1], v[2])));
                    }
            return local.y >= Surface(local).height ? best : -best;
        }

        // The pit sample (below all eight neighbours) nearest the point.
        GlobalSampleIndex Pit(double x, double z) const
        {
            GlobalSampleIndex best{};
            double bestDistance = std::numeric_limits<double>::infinity();
            for (int64_t k = 2 - kIntervals; k <= kIntervals - 2; ++k)
                for (int64_t i = 2 - kIntervals; i <= kIntervals - 2; ++i)
                {
                    bool lowest = true;
                    for (int64_t dk = -1; dk <= 1; ++dk)
                        for (int64_t di = -1; di <= 1; ++di)
                            if ((di != 0 || dk != 0) && Sample({i + di, k + dk}) <= Sample({i, k}))
                                lowest = false;
                    const double d = std::hypot(mLayout.SamplePosition(i) - x, mLayout.SamplePosition(k) - z);
                    if (lowest && d < bestDistance)
                    {
                        best = {i, k};
                        bestDistance = d;
                    }
                }
            REQUIRE(std::isfinite(bestDistance));
            return best;
        }

        const TerrainLayout &Layout() const { return mLayout; }

    private:
        TerrainLayout mLayout;
        WorldPosition mPlacement;
        std::unordered_map<TileCoord, std::vector<float>> mTiles;
    };

    std::optional<RayHit> Down(const PhysicsWorld &world, const glm::dvec3 &above, float length)
    {
        return world.RayCast(At(above), glm::vec3(0.0f, -1.0f, 0.0f), length);
    }

    // 9 x 9 points on every face.
    std::vector<glm::dvec3> BoxPoints(const PhysicsWorld &world, BodyHandle box, const glm::vec3 &half)
    {
        const glm::dvec3 center = MetersOf(world, box);
        const glm::quat rotation = world.BodyRotation(box);
        std::vector<glm::dvec3> points;
        for (int axis = 0; axis < 3; ++axis)
            for (float side : {-1.0f, 1.0f})
                for (int a = 0; a <= 8; ++a)
                    for (int b = 0; b <= 8; ++b)
                    {
                        glm::vec3 p;
                        p[axis] = side;
                        p[(axis + 1) % 3] = -1.0f + 0.25f * a;
                        p[(axis + 2) % 3] = -1.0f + 0.25f * b;
                        points.push_back(center + glm::dvec3(rotation * (p * half)));
                    }
        return points;
    }

    struct Rest
    {
        glm::dvec3 sphere; // terrain-local centre
        glm::dvec3 box;
    };

    Rest DropIntoPit(const glm::dvec3 &placement)
    {
        const Terrain terrain(Hills, placement);
        const GlobalSampleIndex pit = terrain.Pit(40.0, -30.0);
        const glm::dvec3 above{terrain.Layout().SamplePosition(pit.x), terrain.Sample(pit) + 1.5,
                               terrain.Layout().SamplePosition(pit.z)};
        const glm::vec3 half(0.5f, 0.25f, 0.5f);

        Rest rest;
        for (int shape = 0; shape < 2; ++shape)
        {
            INFO((shape == 0 ? "sphere" : "box"));
            PhysicsWorld world;
            const std::vector<float> area = terrain.Area();
            REQUIRE(terrain.AddArea(world, area).IsValid());
            const BodyHandle body = shape == 0 ? world.CreateDynamicSphere(At(terrain.World(above)), 0.5f)
                                               : world.CreateDynamicBox(At(terrain.World(above)), half);
            for (int i = 0; i < 300; ++i)
                world.Step(kStep);

            CHECK(glm::length(world.BodyLinearVelocity(body)) < 0.01f);
            const glm::dvec3 local = terrain.Local(MetersOf(world, body));
            if (shape == 0)
            {
                const double gap = terrain.SignedDistance(local, 2.0) - 0.5;
                CHECK(gap >= -(kPenetration + kHillsQuantization));
                CHECK(gap <= kHillsQuantization);
                rest.sphere = local;
            }
            else
            {
                double lowest = std::numeric_limits<double>::infinity();
                for (const glm::dvec3 &p : BoxPoints(world, body, half))
                    lowest = std::min(lowest, terrain.SignedDistance(terrain.Local(p), 1.0));
                CHECK(lowest >= -(kPenetration + kHillsQuantization));
                CHECK(lowest <= kHillsQuantization + 0.01);
                rest.box = local;
            }
            CHECK(std::abs(local.x - above.x) < kSpacing);
            CHECK(std::abs(local.z - above.z) < kSpacing);
        }
        return rest;
    }

    struct Pass
    {
        double lowestGap = std::numeric_limits<double>::infinity();
        double highestGap = -std::numeric_limits<double>::infinity();
        double largestKick = 0.0; // per-step change of the linear velocity
        int seamCrossings = 0;
        bool fellThrough = false;
    };

    // Shape 0 is a sphere of radius 0.5, 1 a box of half extent (0.5, 0.25, 0.5).
    Pass Slide(const Terrain &terrain, bool perTile, int shape, double heading)
    {
        PhysicsWorld world;
        const std::vector<float> area = terrain.Area();
        if (perTile)
            terrain.AddTiles(world);
        else
            terrain.AddArea(world, area);

        const glm::dvec2 dir(std::cos(heading), std::sin(heading));
        const glm::dvec2 start = glm::dvec2(1.7, 2.3) - dir * 12.0;
        const double norm = std::sqrt(1.0 + kPlaneX * kPlaneX + kPlaneZ * kPlaneZ);
        const double lift = shape == 0 ? 0.5 * norm : 0.25 + 0.5 * (std::abs(kPlaneX) + std::abs(kPlaneZ));
        const glm::dvec3 local{start.x, Plane(start.x, start.y) + lift + 0.005, start.y};
        const glm::vec3 velocity = glm::normalize(glm::vec3(glm::dvec3(dir.x, kPlaneX * dir.x + kPlaneZ * dir.y, dir.y))) * 8.0f;
        const glm::vec3 half(0.5f, 0.25f, 0.5f);
        const BodyHandle body = shape == 0 ? world.CreateDynamicSphere(At(terrain.World(local)), 0.5f)
                                           : world.CreateDynamicBox(At(terrain.World(local)), half);
        world.SetBodyLinearVelocity(body, velocity);

        const auto gapOf = [&](const glm::dvec3 &p) { return (p.y - Plane(p.x, p.z)) / norm; };

        Pass pass;
        glm::vec3 previous = velocity;
        glm::dvec3 previousCentre = local;
        for (int step = 0; step < 150; ++step)
        {
            world.Step(kStep);
            const glm::dvec3 centre = terrain.Local(MetersOf(world, body));
            const glm::vec3 v = world.BodyLinearVelocity(body);
            pass.fellThrough |= gapOf(centre) <= 0.0;
            pass.seamCrossings += (previousCentre.x < 0.0) != (centre.x < 0.0);
            pass.seamCrossings += (previousCentre.z < 0.0) != (centre.z < 0.0);
            if (step >= 45)
            {
                double gap = gapOf(centre) - 0.5;
                if (shape == 1)
                {
                    gap = std::numeric_limits<double>::infinity();
                    for (const glm::dvec3 &p : BoxPoints(world, body, half))
                        gap = std::min(gap, gapOf(terrain.Local(p)));
                }
                pass.lowestGap = std::min(pass.lowestGap, gap);
                pass.highestGap = std::max(pass.highestGap, gap);
                pass.largestKick = std::max(pass.largestKick, double(glm::length(v - previous)));
            }
            previous = v;
            previousCentre = centre;
        }
        return pass;
    }
}

TEST_CASE("A height field is padded with holes, never with ground", "[physics][heightfield]")
{
    const Terrain terrain(Hills, glm::dvec3(0.0));
    PhysicsWorld world;
    const std::vector<BodyHandle> tiles = terrain.AddTiles(world);
    const std::vector<float> area = terrain.Area();
    PhysicsWorld areaWorld;
    const BodyHandle whole = terrain.AddArea(areaWorld, area);

    // 65 samples round up to Jolt's block of 2, 129 likewise.
    REQUIRE(tiles.size() == 4);
    for (const BodyHandle tile : tiles)
    {
        const std::optional<HeightFieldInfo> info = world.HeightFieldInfoOf(tile);
        REQUIRE(info);
        CHECK(info->mSampleCount == 66);
        CHECK(info->mMemoryBytes > 0);
    }
    REQUIRE(areaWorld.HeightFieldInfoOf(whole));
    CHECK(areaWorld.HeightFieldInfoOf(whole)->mSampleCount == 130);
    CHECK(world.BodyCount() == 4);
    CHECK(areaWorld.BodyCount() == 1);

    // Closed edges collide; the padding past the max edges does not.
    std::mt19937_64 random(16);
    std::uniform_real_distribution<double> along(-255.0, 255.0), past(1e-3, 8.0);
    for (int n = 0; n < 200; ++n)
    {
        const double a = along(random), b = past(random);
        for (const glm::dvec3 &outside : {glm::dvec3(256.0 + b, 0.0, a), glm::dvec3(a, 0.0, 256.0 + b),
                                          glm::dvec3(-256.0 - b, 0.0, a), glm::dvec3(a, 0.0, -256.0 - b)})
            CHECK_FALSE(Down(areaWorld, outside + glm::dvec3(0.0, 400.0, 0.0), 800.0f));
    }
    for (const glm::dvec3 &edge : {glm::dvec3(256.0, 0.0, 17.0), glm::dvec3(-256.0, 0.0, 17.0), glm::dvec3(17.0, 0.0, 256.0),
                                   glm::dvec3(256.0, 0.0, 256.0), glm::dvec3(-256.0, 0.0, -256.0)})
        CHECK(Down(areaWorld, edge + glm::dvec3(0.0, 400.0, 0.0), 800.0f));
}

TEST_CASE("Holes and short sides drop exactly the triangles without samples", "[physics][heightfield]")
{
    std::vector<float> samples(9 * 5, 1.0f);
    samples[2 * 9 + 4] = kHeightFieldHole;

    PhysicsWorld world;
    HeightFieldDesc desc;
    desc.mSamples = samples;
    desc.mCountX = 9;
    desc.mCountZ = 5;
    desc.mSpacing = 1.0f;
    const BodyHandle field = world.CreateStaticHeightField(desc);
    REQUIRE(field.IsValid());
    REQUIRE(world.HeightFieldInfoOf(field));
    CHECK(world.HeightFieldInfoOf(field)->mSampleCount == 10);

    const auto hits = [&](double x, double z) {
        const std::optional<RayHit> hit = Down(world, {x, 5.0, z}, 10.0f);
        if (hit)
            CHECK(std::abs(double(hit->mPoint.Meters().y) - 1.0) < 1e-4);
        return hit.has_value();
    };
    CHECK(hits(0.5, 0.5));
    CHECK(hits(7.9, 3.9));
    CHECK(hits(1.5, 2.5));
    // Only the triangles with (4, 2) as a corner are gone (triangle 0 holds dz >= dx).
    CHECK_FALSE(hits(3.5, 1.7)); // cell (3, 1), both triangles
    CHECK_FALSE(hits(3.7, 1.5));
    CHECK_FALSE(hits(4.5, 2.5)); // cell (4, 2), both triangles
    CHECK_FALSE(hits(3.9, 2.1)); // cell (3, 2), triangle 1
    CHECK(hits(3.1, 2.9));       // cell (3, 2), triangle 0
    CHECK_FALSE(hits(4.1, 1.9)); // cell (4, 1), triangle 0
    CHECK(hits(4.9, 1.1));       // cell (4, 1), triangle 1
    // z past 4 and x past 8 are the square's padding.
    CHECK_FALSE(hits(2.5, 4.5));
    CHECK_FALSE(hits(8.5, 2.0));
}

TEST_CASE("Malformed height fields are refused", "[physics][heightfield]")
{
    PhysicsWorld world;
    std::vector<float> samples(5 * 5, 0.0f);
    const auto make = [&](HeightFieldDesc desc) { return world.CreateStaticHeightField(desc); };
    HeightFieldDesc good;
    good.mSamples = samples;
    good.mCountX = 5;
    good.mCountZ = 5;

    HeightFieldDesc d = good;
    d.mCountX = 4; // wrong sample count
    CHECK_FALSE(make(d).IsValid());
    d = good;
    d.mCountX = 1;
    d.mCountZ = 25;
    CHECK_FALSE(make(d).IsValid());
    d = good;
    d.mSampleStride = 3; // 4 intervals do not split into 3
    CHECK_FALSE(make(d).IsValid());
    d.mSampleStride = 0;
    CHECK_FALSE(make(d).IsValid());
    d = good;
    d.mSpacing = 0.0f;
    CHECK_FALSE(make(d).IsValid());
    d.mSpacing = std::numeric_limits<float>::infinity();
    CHECK_FALSE(make(d).IsValid());
    for (const uint32_t block : {0u, 1u, 3u, 8u})
    {
        d = good;
        d.mBlockSize = block;
        CHECK_FALSE(make(d).IsValid());
    }
    for (const uint32_t bits : {0u, 9u})
    {
        d = good;
        d.mBitsPerSample = bits;
        CHECK_FALSE(make(d).IsValid());
    }
    samples[7] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(make(good).IsValid());
    samples[7] = -std::numeric_limits<float>::infinity();
    CHECK_FALSE(make(good).IsValid());

    CHECK(world.BodyCount() == 0);
    CHECK(world.HeightFieldsBuilt() == 0);

    samples[7] = 0.0f;
    HeightFieldDesc coarse = good;
    coarse.mSampleStride = 2;
    coarse.mBlockSize = 4;
    coarse.mBitsPerSample = 6;
    CHECK(make(coarse).IsValid());
    CHECK(world.HeightFieldsBuilt() == 1);
}

TEST_CASE("Rays meet the canonical surface within Jolt's quantization", "[physics][heightfield]")
{
    struct Fixture
    {
        double (*height)(double, double);
        double tolerance;
    };
    for (const Fixture fixture : {Fixture{Hills, kHillsQuantization}, Fixture{Plane, kPlaneQuantization}})
        for (const bool perTile : {false, true})
        {
            INFO((fixture.height == Hills ? "Hills" : "Plane") << (perTile ? ", per tile" : ", whole area"));
            const Terrain terrain(fixture.height, glm::dvec3(0.0));
            PhysicsWorld world;
            const std::vector<float> area = terrain.Area();
            if (perTile)
                terrain.AddTiles(world);
            else
                terrain.AddArea(world, area);

            std::mt19937_64 random(0x16);
            std::uniform_real_distribution<double> unit(-256.0, 256.0);
            double worst = 0.0, worstNormal = 1.0;
            for (int n = 0; n < 4000; ++n)
            {
                const double x = unit(random), z = unit(random);
                const SurfaceSample surface = terrain.Surface({x, 0.0, z});
                // Short rays: the hit fraction is a float.
                const WorldPosition from = At({x, surface.height + 1.0, z});
                const std::optional<RayHit> hit = world.RayCast(from, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f);
                REQUIRE(hit);
                CHECK(hit->mPoint.Meters().x == from.Meters().x);
                CHECK(hit->mPoint.Meters().z == from.Meters().z);
                worst = std::max(worst, std::abs(double(hit->mPoint.Meters().y) - surface.height));
                // Away from cell edges the normal only tilts by the quantization.
                const double fx = x / kSpacing - std::floor(x / kSpacing), fz = z / kSpacing - std::floor(z / kSpacing);
                if (std::abs(fx - fz) > 1e-3 && std::min({fx, fz, 1.0 - fx, 1.0 - fz}) > 1e-3)
                    worstNormal = std::min(worstNormal, glm::dot(glm::dvec3(hit->mNormal), surface.normal));
            }
            CHECK(worst <= fixture.tolerance);
            CHECK(worstNormal >= std::cos(3.0 * 3.14159265358979 / 180.0));
        }
}

TEST_CASE("Rays meet the surface at the map edge exactly as at the origin", "[physics][heightfield][lwc]")
{
    if (!kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // On a 1/1024 m grid every coordinate is exact in double, so Jolt sees the same floats anywhere.
    std::mt19937_64 random(0x16);
    std::uniform_int_distribution<int64_t> grid(-256 * 1024, 256 * 1024);
    std::vector<glm::dvec2> points;
    for (int n = 0; n < 2000; ++n)
        points.emplace_back(double(grid(random)) / 1024.0, double(grid(random)) / 1024.0);

    std::vector<double> errors[3];
    const glm::dvec3 placements[3] = {glm::dvec3(0.0), kEdgeA, kEdgeB};
    for (int p = 0; p < 3; ++p)
    {
        const Terrain terrain(Hills, placements[p]);
        PhysicsWorld world;
        const std::vector<float> area = terrain.Area();
        const BodyHandle field = terrain.AddArea(world, area);
        CHECK(MetersOf(world, field) == glm::dvec3(terrain.SampleOrigin({-kIntervals, -kIntervals}).Meters()));

        for (const glm::dvec2 &point : points)
        {
            const double surface = terrain.Surface({point.x, 0.0, point.y}).height;
            const glm::dvec3 above = terrain.World({point.x, surface + 1.0, point.y});
            const std::optional<RayHit> hit = Down(world, above, 2.0f);
            REQUIRE(hit);
            const glm::dvec3 local = terrain.Local(glm::dvec3(hit->mPoint.Meters()));
            CHECK(local.x == point.x);
            CHECK(local.z == point.y);
            errors[p].push_back(local.y - surface);
        }
    }
    for (int p = 1; p < 3; ++p)
        for (size_t n = 0; n < points.size(); ++n)
            CHECK(std::abs(errors[p][n] - errors[0][n]) <= 1e-9);
}

TEST_CASE("A sphere and a box come to rest on the surface", "[physics][heightfield]")
{
    DropIntoPit(glm::dvec3(0.0));
}

TEST_CASE("Bodies rest on a far height field as they do at the origin", "[physics][heightfield][lwc]")
{
    if (!kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const Rest near = DropIntoPit(glm::dvec3(0.0));
    for (const glm::dvec3 &edge : {kEdgeA, kEdgeB})
    {
        const Rest far = DropIntoPit(edge);
        CHECK(glm::length(far.sphere - near.sphere) <= 1e-4);
        CHECK(glm::length(far.box - near.box) <= 1e-4);
    }
}

TEST_CASE("Bodies cross tile edges inside one height field without bumps", "[physics][heightfield]")
{
    const Terrain terrain(Plane, glm::dvec3(0.0));
    int crossed = 0;
    for (int shape = 0; shape < 2; ++shape)
        for (int n = 0; n < 12; ++n)
        {
            const double heading = n * 2.0 * 3.14159265358979 / 12.0;
            INFO((shape == 0 ? "sphere" : "box") << ", heading " << n * 30 << " degrees");
            const Pass pass = Slide(terrain, false, shape, heading);
            crossed += pass.seamCrossings > 0;
            CHECK_FALSE(pass.fellThrough);
            CHECK(pass.lowestGap >= -(kPenetration + kPlaneQuantization));
            CHECK(pass.highestGap <= 0.005);
            // ~0.03 m/s per step from gravity and friction; a snag is metres per second.
            CHECK(pass.largestKick <= 0.3);
        }
    CHECK(crossed >= 12);
}

TEST_CASE("Per-tile height fields hold bodies up at their seams", "[physics][heightfield]")
{
    // Per-tile shapes snag boxes at seams, but nothing falls through.
    const Terrain terrain(Plane, glm::dvec3(0.0));
    for (int shape = 0; shape < 2; ++shape)
        for (int n = 0; n < 12; ++n)
        {
            INFO((shape == 0 ? "sphere" : "box") << ", heading " << n * 30 << " degrees");
            const Pass pass = Slide(terrain, true, shape, n * 2.0 * 3.14159265358979 / 12.0);
            CHECK_FALSE(pass.fellThrough);
            CHECK(pass.lowestGap >= -0.05);
        }
}

TEST_CASE("Stepping never rebuilds a height field", "[physics][heightfield]")
{
    const Terrain terrain(Hills, glm::dvec3(0.0));
    PhysicsWorld world;
    const std::vector<float> area = terrain.Area();
    const BodyHandle field = terrain.AddArea(world, area);
    const std::optional<HeightFieldInfo> before = world.HeightFieldInfoOf(field);
    REQUIRE(before);
    CHECK(world.HeightFieldsBuilt() == 1);

    const BodyHandle ball = world.CreateDynamicSphere(At({10.0, terrain.Surface({10.0, 0.0, 10.0}).height + 2.0, 10.0}), 0.5f);
    world.SetBodyLinearVelocity(ball, glm::vec3(4.0f, 0.0f, 3.0f));
    for (int i = 0; i < 120; ++i)
        world.Step(kStep);

    CHECK(world.HeightFieldsBuilt() == 1);
    const std::optional<HeightFieldInfo> after = world.HeightFieldInfoOf(field);
    REQUIRE(after);
    CHECK(after->mSampleCount == before->mSampleCount);
    CHECK(after->mMemoryBytes == before->mMemoryBytes);
    CHECK_FALSE(world.HeightFieldInfoOf(ball));
    CHECK_FALSE(world.HeightFieldInfoOf(BodyHandle{}));
}

TEST_CASE("Destroying a height field between steps drops what rested on it", "[physics][heightfield]")
{
    const Terrain terrain(Plane, glm::dvec3(0.0));
    PhysicsWorld world;
    const std::vector<float> area = terrain.Area();
    const BodyHandle field = terrain.AddArea(world, area);
    const glm::dvec3 spot{10.5, 0.0, 20.5};
    const BodyHandle box = world.CreateDynamicBox(At({spot.x, Plane(spot.x, spot.z) + 0.6, spot.z}), glm::vec3(0.5f, 0.25f, 0.5f));

    for (int i = 0; i < 240; ++i)
        world.Step(kStep);
    const double restY = MetersOf(world, box).y;
    CHECK(glm::length(world.BodyLinearVelocity(box)) == 0.0f);

    world.DestroyBody(field);
    CHECK(world.BodyCount() == 1);
    CHECK_FALSE(world.HeightFieldInfoOf(field));
    CHECK_FALSE(Down(world, {spot.x + 3.0, 100.0, spot.z}, 200.0f));
    world.DestroyBody(field);
    CHECK(world.BodyCount() == 1);

    for (int i = 0; i < 60; ++i)
        world.Step(kStep);
    // Free fall moves ~4.9 m in a second; a box left asleep would not move.
    CHECK(MetersOf(world, box).y < restY - 3.0);
}

TEST_CASE("A sample stride builds the collider from every stride-th sample", "[physics][heightfield]")
{
    for (const bool hills : {false, true})
    {
        INFO((hills ? "Hills" : "Plane"));
        const Terrain terrain(hills ? Hills : Plane, glm::dvec3(0.0));
        PhysicsWorld world;
        const std::vector<float> area = terrain.Area();
        HeightFieldDesc desc;
        desc.mSampleStride = 2;
        const BodyHandle field = terrain.AddArea(world, area, desc);
        REQUIRE(world.HeightFieldInfoOf(field));
        CHECK(world.HeightFieldInfoOf(field)->mSampleCount == 66);

        double worst = 0.0;
        for (int64_t k = -kIntervals; k <= kIntervals; k += 2)
            for (int64_t i = -kIntervals; i <= kIntervals; i += 2)
            {
                const double x = i * kSpacing, z = k * kSpacing;
                const double h = terrain.Sample({i, k});
                const std::optional<RayHit> hit = Down(world, {x, h + 1.0, z}, 2.0f);
                REQUIRE(hit);
                worst = std::max(worst, std::abs(double(hit->mPoint.Meters().y) - h));
            }
        CHECK(worst <= (hills ? 2.0 * kHillsQuantization : 2.0 * kPlaneQuantization));
    }
}

TEST_CASE("One built height field shape serves static bodies at several places", "[physics][heightfield]")
{
    const Terrain terrain(Hills, glm::dvec3(0.0));
    const std::vector<float> area = terrain.Area();
    HeightFieldDesc desc;
    desc.mSamples = area;
    desc.mCountX = kAreaSide;
    desc.mCountZ = kAreaSide;
    desc.mSpacing = static_cast<float>(kSpacing);
    const SharedHeightField shape = BuildHeightField(desc);
    REQUIRE(shape.IsValid());
    REQUIRE(shape.Info());

    PhysicsWorld world;
    const glm::dvec3 offsets[2] = {glm::dvec3(-1000.0, 5.0, 0.0), glm::dvec3(1000.0, -3.0, 600.0)};
    BodyHandle bodies[2];
    for (int i = 0; i < 2; ++i)
    {
        bodies[i] = world.CreateStaticHeightField(shape, At(offsets[i]));
        REQUIRE(bodies[i].IsValid());
        const std::optional<HeightFieldInfo> info = world.HeightFieldInfoOf(bodies[i]);
        REQUIRE(info);
        CHECK(info->mSampleCount == shape.Info()->mSampleCount);
        CHECK(info->mMaxQuantizationError == shape.Info()->mMaxQuantizationError);
    }
    CHECK(world.HeightFieldsBuilt() == 0);

    for (const glm::dvec2 at : {glm::dvec2(10.0, 20.0), glm::dvec2(300.5, 17.25), glm::dvec2(511.0, 1.0)})
    {
        double heights[2];
        for (int i = 0; i < 2; ++i)
        {
            const std::optional<RayHit> hit = Down(world, offsets[i] + glm::dvec3(at.x, 400.0, at.y), 600.0f);
            REQUIRE(hit);
            CHECK(hit->mBody == bodies[i]);
            heights[i] = double(hit->mPoint.Meters().y) - offsets[i].y;
        }
        CHECK(std::abs(heights[0] - heights[1]) <= 1e-3);
    }

    world.DestroyBody(bodies[0]);
    CHECK_FALSE(Down(world, offsets[0] + glm::dvec3(10.0, 400.0, 20.0), 600.0f));
    CHECK(Down(world, offsets[1] + glm::dvec3(10.0, 400.0, 20.0), 600.0f));
}

TEST_CASE("Shapes given one quantization range decode the samples they share alike away from their max sides",
          "[physics][heightfield]")
{
    const Terrain terrain(Hills, glm::dvec3(0.0));
    const std::vector<float> area = terrain.Area();
    const float low = *std::min_element(area.begin(), area.end());
    const float high = *std::max_element(area.begin(), area.end());

    const auto block = [&](uint32_t first, uint32_t last)
    {
        std::vector<float> samples;
        for (uint32_t z = 0; z < kAreaSide; ++z)
            for (uint32_t x = first; x <= last; ++x)
                samples.push_back(area[size_t(z) * kAreaSide + x]);
        return samples;
    };
    const std::vector<float> west = block(0, 80), east = block(64, 128);
    for (const bool shared : {false, true})
    {
        INFO((shared ? "one range" : "own ranges"));
        const auto build = [&](const std::vector<float> &samples, uint32_t columns)
        {
            HeightFieldDesc desc;
            desc.mSamples = samples;
            desc.mCountX = columns;
            desc.mCountZ = kAreaSide;
            desc.mSpacing = static_cast<float>(kSpacing);
            if (shared)
            {
                desc.mQuantizationMin = low;
                desc.mQuantizationMax = high;
            }
            return BuildHeightField(desc);
        };
        const SharedHeightField a = build(west, 81), b = build(east, 65);
        REQUIRE(a.IsValid());
        REQUIRE(b.IsValid());
        PhysicsWorld wa, wb;
        REQUIRE(wa.CreateStaticHeightField(a, At({0.0, 0.0, 0.0})).IsValid());
        REQUIRE(wb.CreateStaticHeightField(b, At({64 * kSpacing, 0.0, 0.0})).IsValid());

        double worst = 0.0;
        for (uint32_t z = 0; z + 2 < kAreaSide; ++z)
            for (uint32_t x = 64; x + 2 <= 80; ++x)
            {
                const glm::dvec3 above{x * kSpacing, area[size_t(z) * kAreaSide + x] + 1.0, z * kSpacing};
                const std::optional<RayHit> ha = Down(wa, above, 2.0f), hb = Down(wb, above, 2.0f);
                REQUIRE(ha);
                REQUIRE(hb);
                worst = std::max(worst, std::abs(double(ha->mPoint.Meters().y) - double(hb->mPoint.Meters().y)));
            }
        if (shared)
            CHECK(worst <= 1e-4);
        else
            CHECK(worst > 1e-3);
    }

    HeightFieldDesc outside;
    outside.mSamples = west;
    outside.mCountX = 81;
    outside.mCountZ = kAreaSide;
    outside.mSpacing = static_cast<float>(kSpacing);
    outside.mQuantizationMin = low + 1.0f;
    outside.mQuantizationMax = high;
    CHECK_FALSE(BuildHeightField(outside).IsValid());
    outside.mQuantizationMin = high;
    outside.mQuantizationMax = low;
    CHECK_FALSE(BuildHeightField(outside).IsValid());
}
