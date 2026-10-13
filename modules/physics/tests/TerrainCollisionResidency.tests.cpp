/**
 * @file TerrainCollisionResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainCollisionResidency driving physics/PhysicsWorld bodies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../../terrain/tests/TerrainPackTestSupport.h"
#include "../../terrain/tests/TerrainStreamingTestSupport.h"

#include <physics/PhysicsWorld.h>
#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr double kPi = 3.14159265358979323846;
    // Jolt's resting penetration plus its height field quantization on these grounds.
    constexpr double kSink = 0.06;

    using Height = double (*)(double, double);

    double Ridges(double x, double z)
    {
        return 15.0 + 12.0 * std::abs(std::sin(0.012 * x + 0.3)) * std::cos(0.009 * z) + 0.5 * std::sin(0.2 * x);
    }

    constexpr double kPlaneX = -0.08, kPlaneZ = -0.03;
    double Plane(double x, double z) { return 50.0 + kPlaneX * x + kPlaneZ * z; }

    class JoltShape final : public TerrainColliderShape
    {
    public:
        explicit JoltShape(SharedHeightField field) : field(std::move(field)) {}
        SharedHeightField field;
    };

    // The physics side of terrain collision over one PhysicsWorld.
    class JoltColliders final : public TerrainColliderPhysics
    {
    public:
        explicit JoltColliders(PhysicsWorld &world) : mWorld(world) {}

        std::optional<TerrainBuiltCollider> Build(const TerrainAreaSamples &samples, float quantizationMin,
                                                  float quantizationMax) const override
        {
            HeightFieldDesc desc;
            desc.mSamples = samples.heights;
            desc.mCountX = samples.countX;
            desc.mCountZ = samples.countZ;
            desc.mSpacing = static_cast<float>(samples.spacing);
            desc.mQuantizationMin = quantizationMin;
            desc.mQuantizationMax = quantizationMax;
            SharedHeightField field = BuildHeightField(desc);
            if (!field.Info())
                return std::nullopt;
            const HeightFieldInfo info = *field.Info();
            return TerrainBuiltCollider{std::make_shared<JoltShape>(std::move(field)), info.mMaxQuantizationError,
                                        info.mMaxHorizontalError, info.mExtentX, info.mExtentZ, info.mMemoryBytes};
        }

        TerrainColliderId Place(const TerrainColliderShape &shape, WorldPosition origin, const TerrainPlacement &) override
        {
            const BodyHandle body = mWorld.CreateStaticHeightField(static_cast<const JoltShape &>(shape).field, origin);
            if (!body.IsValid())
                return 0;
            mWorld.RestrictBody(body);
            return TerrainColliderId(body.mId) + 1;
        }

        void Remove(TerrainColliderId collider) override { mWorld.DestroyBody(BodyHandle{uint32_t(collider - 1)}); }

    private:
        PhysicsWorld &mWorld;
    };

    struct Body
    {
        BodyHandle handle;
        double radius = 0.0;
        // Zero for a sphere.
        glm::vec3 half{0.0f};
        double tolerance = 0.05;
        int heldSteps = 0;
        double lowestGap = std::numeric_limits<double>::infinity();
        bool fell = false;
    };

    TerrainEncodedRelease Release(const TerrainReleaseInfo &info, const TerrainLayout &layout, Height height,
                                  std::vector<TileHeights> &tiles)
    {
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TileHeights tile = *TileHeights::Create({x, z}, n);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        tile.Set(i, k, static_cast<float>(height(layout.SamplePosition(layout.TileFirstSample(x) + i),
                                                                 layout.SamplePosition(layout.TileFirstSample(z) + k))));
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

    // The residency, the streamer and one PhysicsWorld driven a fixed step at a time.
    struct Scene
    {
        ManualClock clock;
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        TerrainStreamingSettings settings;
        std::unique_ptr<TerrainStreamer> streamer;
        PhysicsWorld world;
        std::shared_ptr<JoltColliders> colliders = std::make_shared<JoltColliders>(world);
        std::unique_ptr<TerrainCollisionResidency> residency;
        TerrainReleaseInfo info;
        TerrainLayout layout;
        std::vector<TileHeights> tiles;
        ControlledRelease release;
        glm::dvec3 placement{0.0};
        std::vector<Body> bodies;
        int steps = 0;
        // Reads and builds wait while set, as on a slow disk.
        bool paused = false;

        Scene(TerrainResidencyMode mode, Height height, TerrainLayout terrain, uint32_t pieceTiles, double overlap,
              std::vector<double> spacings = {})
            : settings(StreamingSettings(uint64_t(1) << 30, mode)),
              info(Info(TerrainAsset("terrain/collision-bodies.terrain.json"), 0xB0D1)), layout(terrain)
        {
            if (!spacings.empty())
                settings.collision.spacingsMeters = std::move(spacings);
            settings.collision.pieceTiles = pieceTiles;
            settings.collision.seamOverlapMeters = overlap;
            REQUIRE(ValidateTerrainStreamingSettings(settings).empty());
            streamer = std::make_unique<TerrainStreamer>(settings, executor, clock.Function());
            residency = std::make_unique<TerrainCollisionResidency>(settings.collision, executor, colliders);
            release = OpenControlled(Release(info, layout, height, tiles));
            streamer->Add(release.release);
        }

        void Place(const glm::dvec3 &at)
        {
            placement = at;
            const std::vector<TerrainCollisionInstance> instances{
                {release.release->SharedHandle(), {info.asset, At(at)}, *streamer->ModeOf(info)}};
            residency->SetInstances(instances);
        }

        double Surface(double x, double z) const
        {
            const auto local = TerrainLocalPosition::FromMeters({x - placement.x, 0.0, z - placement.z});
            REQUIRE(local);
            const SurfaceSample sample = EvaluateSurface(layout, *local, [this](TileCoord tile) {
                for (const TileHeights &t : tiles)
                    if (t.Tile() == tile)
                        return t.Samples();
                return std::span<const float>();
            });
            REQUIRE(sample.IsOk());
            return placement.y + sample.height;
        }

        size_t AddSphere(const glm::dvec3 &at, double radius, const glm::vec3 &velocity = glm::vec3(0.0f))
        {
            Body body;
            body.handle = world.CreateDynamicSphere(At(at), float(radius));
            world.SetBodyLinearVelocity(body.handle, velocity);
            body.radius = radius;
            bodies.push_back(body);
            return bodies.size() - 1;
        }

        size_t AddBox(const glm::dvec3 &at, const glm::vec3 &half, const glm::vec3 &velocity)
        {
            Body body;
            body.handle = world.CreateDynamicBox(At(at), half);
            world.SetBodyLinearVelocity(body.handle, velocity);
            body.radius = glm::length(half);
            body.half = half;
            bodies.push_back(body);
            return bodies.size() - 1;
        }

        glm::dvec3 Position(size_t body) const
        {
            const std::optional<WorldPosition> at = world.BodyPosition(bodies[body].handle);
            REQUIRE(at);
            return glm::dvec3(at->Meters());
        }

        double Now() const { return *clock.now; }

        // One frame of one fixed step.
        void Frame()
        {
            streamer->Update();
            residency->Install(Now());
            std::vector<TerrainCollisionSource> sources;
            for (const Body &body : bodies)
            {
                TerrainCollisionSource source;
                source.position = *world.BodyPosition(body.handle);
                source.velocity = glm::dvec3(world.BodyLinearVelocity(body.handle));
                source.radius = body.radius;
                source.acceleration = glm::length(world.Gravity());
                source.tolerance = body.tolerance;
                sources.push_back(source);
            }
            residency->Update(sources, *streamer, Now(), kStep);
            for (size_t i = 0; i < bodies.size(); ++i)
            {
                const TerrainCollisionAnswer &answer = residency->Answer(i);
                const bool simulate = answer.status.state == TerrainCollisionState::Ready ||
                                      answer.status.state == TerrainCollisionState::CoarseSufficient;
                if (simulate)
                    world.ResumeBody(bodies[i].handle);
                else
                    world.HoldBody(bodies[i].handle);
                std::vector<BodyHandle> contacts;
                for (const TerrainColliderId id : answer.contacts)
                    contacts.push_back(BodyHandle{uint32_t(id - 1)});
                world.RestrictContacts(bodies[i].handle, contacts);
            }
            world.Step(kStep);
            if (!paused)
                executor->RunAll();
            clock.Advance(kStep);
            ++steps;
            for (Body &body : bodies)
            {
                body.heldSteps += world.IsBodyHeld(body.handle);
                const std::optional<WorldPosition> at = world.BodyPosition(body.handle);
                REQUIRE(at);
                const glm::dvec3 p(at->Meters());
                double gap = p.y - body.radius - Surface(p.x, p.z);
                if (body.half != glm::vec3(0.0f))
                {
                    const glm::quat rotation = world.BodyRotation(body.handle);
                    gap = std::numeric_limits<double>::infinity();
                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const glm::vec3 sign((corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f,
                                             (corner & 4) ? 1.0f : -1.0f);
                        const glm::dvec3 c = p + glm::dvec3(rotation * (sign * body.half));
                        gap = std::min(gap, c.y - Surface(c.x, c.z));
                    }
                }
                body.lowestGap = std::min(body.lowestGap, gap);
                body.fell = body.fell || gap < -body.radius;
            }
        }

        void Run(int frames)
        {
            for (int i = 0; i < frames; ++i)
                Frame();
        }
    };

    TerrainLayout Ground() { return *TerrainLayout::CreateFromTileSize(64.0, 4.0, {-256.0, -256.0, 256.0, 256.0}); }

    // P1's crossing fixture.
    TerrainLayout Crossing() { return *TerrainLayout::CreateFromTileSize(256.0, 4.0, {-256.0, -256.0, 256.0, 256.0}); }

    struct Pass
    {
        double lowestGap = std::numeric_limits<double>::infinity();
        double highestGap = -std::numeric_limits<double>::infinity();
        double kick = 0.0;
        int crossings = 0;
        int unowned = 0;
        // Steps whose contacts named other pieces than the step before.
        int handovers = 0;
    };

    // P1's slide over the plane, the body asking the residency for its pieces.
    Pass Slide(TerrainResidencyMode mode, int shape, double heading, glm::dvec2 site, double lead, int steps,
               double overlap, glm::dvec2 nudge = glm::dvec2(0.0))
    {
        Scene scene(mode, Plane, Crossing(), 1, overlap, {4.0});
        scene.Place({0.0, 0.0, 0.0});
        const glm::dvec2 dir(std::cos(heading), std::sin(heading));
        const glm::dvec2 start = site - dir * lead + nudge;
        const double norm = std::sqrt(1.0 + kPlaneX * kPlaneX + kPlaneZ * kPlaneZ);
        const double lift = shape == 0 ? 0.5 * norm : 0.25 + 0.5 * (std::abs(kPlaneX) + std::abs(kPlaneZ));
        const glm::dvec3 from{start.x, Plane(start.x, start.y) + lift + 0.005, start.y};
        const glm::vec3 velocity =
            glm::normalize(glm::vec3(glm::dvec3(dir.x, kPlaneX * dir.x + kPlaneZ * dir.y, dir.y))) * 8.0f;
        const glm::vec3 half(0.5f, 0.25f, 0.5f);
        const size_t body = shape == 0 ? scene.AddSphere(from, 0.5, velocity) : scene.AddBox(from, half, velocity);
        scene.bodies[body].tolerance = 0.0;
        while (scene.bodies[body].heldSteps == scene.steps)
        {
            scene.Frame();
            REQUIRE(scene.steps < 600);
        }
        const auto gapOf = [&](const glm::dvec3 &p) { return (p.y - Plane(p.x, p.z)) / norm; };
        Pass pass;
        glm::vec3 previous = scene.world.BodyLinearVelocity(scene.bodies[body].handle);
        glm::dvec3 centre = scene.Position(body);
        std::vector<TerrainColliderId> contacts = scene.residency->Answer(0).contacts;
        for (int step = 0; step < steps; ++step)
        {
            scene.Frame();
            REQUIRE_FALSE(scene.world.IsBodyHeld(scene.bodies[body].handle));
            const glm::dvec3 next = scene.Position(body);
            const glm::vec3 v = scene.world.BodyLinearVelocity(scene.bodies[body].handle);
            for (const double line : {0.0, overlap})
            {
                pass.crossings += (centre.x < line) != (next.x < line);
                pass.crossings += (centre.z < line) != (next.z < line);
            }
            pass.unowned += scene.residency->Answer(0).unowned;
            pass.handovers += scene.residency->Answer(0).contacts != contacts;
            contacts = scene.residency->Answer(0).contacts;
            if (step >= 44)
            {
                double gap = gapOf(next) - 0.5;
                if (shape == 1)
                {
                    gap = std::numeric_limits<double>::infinity();
                    const glm::quat rotation = scene.world.BodyRotation(scene.bodies[body].handle);
                    for (int axis = 0; axis < 3; ++axis)
                        for (float side : {-1.0f, 1.0f})
                            for (int a = 0; a <= 8; ++a)
                                for (int b = 0; b <= 8; ++b)
                                {
                                    glm::vec3 p;
                                    p[axis] = side;
                                    p[(axis + 1) % 3] = -1.0f + 0.25f * a;
                                    p[(axis + 2) % 3] = -1.0f + 0.25f * b;
                                    gap = std::min(gap, gapOf(next + glm::dvec3(rotation * (p * half))));
                                }
                }
                pass.lowestGap = std::min(pass.lowestGap, gap);
                pass.highestGap = std::max(pass.highestGap, gap);
                pass.kick = std::max(pass.kick, double(glm::length(v - previous)));
            }
            previous = v;
            centre = next;
        }
        return pass;
    }
}

TEST_CASE("A body over Eager terrain is held until its height field is placed, then rests on it",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Eager, Ridges, Ground(), 2, 64.0);
    scene.Place({1000.25, -40.0, -500.5});
    scene.paused = true;
    const glm::dvec3 start{1010.25, scene.Surface(1010.25, -490.5) + 3.0, -490.5};
    const size_t sphere = scene.AddSphere(start, 0.5);

    scene.Run(60);
    CHECK(scene.bodies[sphere].heldSteps == 60);
    CHECK(scene.Position(sphere) == glm::dvec3(start));
    CHECK(scene.residency->Answer(sphere).status.state == TerrainCollisionState::Pending);

    scene.paused = false;
    scene.Run(300);
    CHECK_FALSE(scene.world.IsBodyHeld(scene.bodies[sphere].handle));
    CHECK(scene.residency->Answer(sphere).status.state == TerrainCollisionState::Ready);
    CHECK_FALSE(scene.bodies[sphere].fell);
    CHECK(scene.bodies[sphere].lowestGap >= -kSink);
    const glm::dvec3 rest = scene.Position(sphere);
    CHECK(std::abs(rest.y - 0.5 - scene.Surface(rest.x, rest.z)) <= kSink);
}

TEST_CASE("A body rests on release A while B's height field is built beside it, and on B once switched to",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Eager, Ridges, Ground(), 2, 64.0);
    const glm::dvec3 placement{1000.25, -40.0, -500.5};
    scene.Place(placement);
    const glm::dvec3 start{1010.25, scene.Surface(1010.25, -490.5) + 1.0, -490.5};
    const size_t sphere = scene.AddSphere(start, 0.5);
    scene.Run(240);
    const glm::dvec3 onA = scene.Position(sphere);
    REQUIRE(std::abs(onA.y - 0.5 - scene.Surface(onA.x, onA.z)) <= kSink);

    std::vector<TileHeights> tilesB;
    const TerrainReleaseInfo infoB = Info(scene.info.asset, 0xB0D2);
    const ControlledRelease b = OpenControlled(Release(
        infoB, scene.layout, [](double x, double z) { return Ridges(x, z) - 1.0; }, tilesB));
    scene.streamer->Add(b.release, true);
    const TerrainPlacement where{scene.info.asset, At(placement)};
    const TerrainCollisionInstance active{scene.release.release->SharedHandle(), where, TerrainResidencyMode::Eager};
    const TerrainCollisionInstance staged{b.release->SharedHandle(), where, TerrainResidencyMode::Eager};
    scene.residency->SetInstances({&active, 1}, {&staged, 1});
    scene.Run(180);
    REQUIRE(scene.residency->StagedStatus(infoB).state == TerrainCollisionState::Ready);
    const glm::dvec3 stillOnA = scene.Position(sphere);
    CHECK(std::abs(stillOnA.y - 0.5 - scene.Surface(stillOnA.x, stillOnA.z)) <= kSink);

    scene.streamer->Add(b.release);
    scene.residency->SetInstances({&staged, 1});
    scene.Run(240);
    CHECK(scene.residency->Answer(sphere).status.state == TerrainCollisionState::Ready);
    const glm::dvec3 onB = scene.Position(sphere);
    const SurfaceSample surfaceB = EvaluateSurface(
        scene.layout, *TerrainLocalPosition::FromMeters(onB - placement),
        [&](TileCoord tile)
        {
            for (const TileHeights &t : tilesB)
                if (t.Tile() == tile)
                    return t.Samples();
            return std::span<const float>();
        });
    REQUIRE(surfaceB.IsOk());
    CHECK(std::abs(onB.y - 0.5 - (placement.y + surfaceB.height)) <= kSink);
    CHECK(onB.y < onA.y - 0.5);
}

TEST_CASE("A fast descent toward raised ground is held short of unloaded terrain, never let through it",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Interest, Ridges, Ground(), 2, 64.0);
    scene.Place({0.0, 0.0, 0.0});
    scene.paused = true;
    const size_t sphere = scene.AddSphere({-70.0, 150.0, 20.0}, 2.0, glm::vec3(0.0f, -30.0f, 0.0f));

    double heldAt = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 300; ++i)
    {
        scene.Frame();
        if (scene.world.IsBodyHeld(scene.bodies[sphere].handle))
            heldAt = std::min(heldAt, scene.Position(sphere).y);
    }
    REQUIRE(std::isfinite(heldAt));
    CHECK(scene.bodies[sphere].heldSteps > 60);
    CHECK(heldAt - 2.0 > scene.Surface(-70.0, 20.0));
    CHECK(scene.world.BodyLinearVelocity(scene.bodies[sphere].handle).y < -30.0f);

    scene.paused = false;
    scene.Run(600);
    CHECK_FALSE(scene.bodies[sphere].fell);
    // Jolt's discrete contact lets an impact sink in for a step or two.
    CHECK(scene.bodies[sphere].lowestGap >= -0.25 * scene.bodies[sphere].radius);
    const glm::dvec3 end = scene.Position(sphere);
    CHECK(end.y - 2.0 - scene.Surface(end.x, end.z) >= -kSink);
    std::cout << "descent,held at," << heldAt - scene.Surface(-70.0, 20.0) << ",held steps,"
              << scene.bodies[sphere].heldSteps << ",lowest gap," << scene.bodies[sphere].lowestGap << '\n';
}

TEST_CASE("Bodies running low over ridges and turning hard keep collision ahead of them",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Interest, Ridges, Ground(), 2, 64.0);
    scene.Place({0.0, 0.0, 0.0});
    const size_t ball = scene.AddSphere({150.0, scene.Surface(150.0, 0.0) + 0.6, 0.0}, 0.5);
    const size_t box = scene.AddBox({-150.0, scene.Surface(-150.0, 0.0) + 0.6, 0.0}, {0.5f, 0.25f, 0.5f}, glm::vec3(0.0f));
    scene.Run(120);
    double angle[2] = {0.0, kPi};
    for (int i = 0; i < 900; ++i)
    {
        const double turn = (i / 120) % 2 == 0 ? 1.0 : -1.0;
        int b = 0;
        for (const size_t body : {ball, box})
        {
            angle[b] += turn * 25.0 / 150.0 * kStep;
            const glm::vec3 v = scene.world.BodyLinearVelocity(scene.bodies[body].handle);
            const glm::vec3 along = glm::vec3(-std::sin(angle[b]), 0.0f, std::cos(angle[b])) * float(25.0 * turn);
            scene.world.SetBodyLinearVelocity(scene.bodies[body].handle, glm::vec3(along.x, v.y, along.z));
            ++b;
        }
        scene.Frame();
    }
    for (const size_t body : {ball, box})
    {
        CHECK_FALSE(scene.bodies[body].fell);
        CHECK(scene.bodies[body].lowestGap >= -kSink);
        std::cout << "turning," << body << ",held steps," << scene.bodies[body].heldSteps << ",lowest gap,"
                  << scene.bodies[body].lowestGap << '\n';
    }
    const TerrainCollisionStats stats = scene.residency->Stats();
    CHECK(stats.ready >= 2);
    std::cout << "turning,pieces," << stats.ready << ",built," << stats.builds << ",retired," << stats.retired
              << ",max latency," << stats.maxLatencySeconds << '\n';
}

TEST_CASE("A body whose collision failed is held while others keep simulating, until a retry loads it",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Interest, Ridges, Ground(), 2, 64.0);
    scene.Place({0.0, 0.0, 0.0});
    scene.release.source->FailNext("chunk_-1_-1.tpack", ~uint32_t(0));
    const size_t near = scene.AddSphere({100.0, scene.Surface(100.0, 100.0) + 2.0, 100.0}, 0.5);
    const size_t also = scene.AddSphere({104.0, scene.Surface(104.0, 98.0) + 2.0, 98.0}, 0.5);
    const glm::dvec3 start{-100.0, scene.Surface(-100.0, -100.0) + 2.0, -100.0};
    const size_t broken = scene.AddSphere(start, 0.5);

    const double nearStart = scene.Position(near).y;
    scene.Run(240);
    CHECK(scene.residency->Answer(broken).status.state == TerrainCollisionState::Failed);
    CHECK(scene.Position(broken) == start);
    CHECK(scene.bodies[near].heldSteps < 30);
    CHECK(scene.bodies[also].heldSteps < 30);
    CHECK(scene.Position(near).y < nearStart - 1.0);
    CHECK(scene.residency->Answer(near).contacts == scene.residency->Answer(also).contacts);

    scene.release.source->FailNext("chunk_-1_-1.tpack", 0);
    scene.clock.Advance(scene.settings.retry.failedHoldSeconds);
    scene.Run(300);
    CHECK(scene.residency->Answer(broken).status.state == TerrainCollisionState::Ready);
    for (const size_t body : {near, also, broken})
    {
        CHECK_FALSE(scene.bodies[body].fell);
        CHECK(scene.bodies[body].lowestGap >= -kSink);
    }
    CHECK(scene.Position(broken).y < start.y - 1.0);
}

TEST_CASE("A teleport into unloaded terrain holds the body until collision there is ready",
          "[physics][terrain][collision-residency]")
{
    Scene scene(TerrainResidencyMode::Interest, Ridges, Ground(), 2, 64.0);
    scene.Place({0.0, 0.0, 0.0});
    const size_t sphere = scene.AddSphere({100.0, scene.Surface(100.0, 100.0) + 1.0, 100.0}, 0.5);
    scene.Run(180);
    const glm::dvec3 landed = scene.Position(sphere);
    CHECK(std::abs(landed.y - 0.5 - scene.Surface(landed.x, landed.z)) <= kSink);

    scene.paused = true;
    const glm::dvec3 target{-180.0, scene.Surface(-180.0, -170.0) + 0.75, -170.0};
    scene.world.SetBodyTransform(scene.bodies[sphere].handle, At(target), kIdentityRotation);
    const int before = scene.bodies[sphere].heldSteps;
    scene.Run(60);
    CHECK(scene.bodies[sphere].heldSteps - before == 60);
    CHECK(scene.Position(sphere) == target);

    scene.paused = false;
    scene.Run(240);
    CHECK_FALSE(scene.bodies[sphere].fell);
    CHECK(scene.bodies[sphere].lowestGap >= -kSink);
    const glm::dvec3 rest = scene.Position(sphere);
    CHECK(std::abs(rest.y - 0.5 - scene.Surface(rest.x, rest.z)) <= kSink);
}

namespace
{
    struct Path
    {
        glm::dvec2 site;
        double lead;
        int steps;
        double overlap;
    };

    struct Range
    {
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();

        void Add(double value)
        {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    };

    Pass Envelope(TerrainResidencyMode mode, const Path &path, glm::dvec2 nudge, int *crossed = nullptr,
                  int *handed = nullptr, int *unowned = nullptr)
    {
        Pass envelope;
        for (int shape = 0; shape < 2; ++shape)
            for (int n = 0; n < 12; ++n)
            {
                INFO((shape == 0 ? "sphere" : "box") << ", heading " << n);
                const Pass pass =
                    Slide(mode, shape, n * 2.0 * kPi / 12.0, path.site, path.lead, path.steps, path.overlap, nudge);
                CHECK(pass.kick <= 0.3);
                envelope.kick = std::max(envelope.kick, pass.kick);
                envelope.lowestGap = std::min(envelope.lowestGap, pass.lowestGap);
                envelope.highestGap = std::max(envelope.highestGap, pass.highestGap);
                if (crossed)
                    *crossed += pass.crossings > 0;
                if (handed)
                    *handed += pass.handovers > 0;
                if (unowned)
                    *unowned += pass.unowned;
            }
        return envelope;
    }
}

TEST_CASE("Bodies cross overlapping height field pieces as smoothly as one shape", "[physics][terrain][collision-seams]")
{
    // P1's slide through the four-tile corner first; then across each hand-over band, then along wider bands.
    const Path paths[] = {{{1.7, 2.3}, 12.0, 149, 24.0},
                          {{12.3, 2.3}, 12.0, 149, 24.0},
                          {{2.3, 12.3}, 12.0, 149, 24.0},
                          {{16.3, 100.3}, 32.0, 479, 32.0},
                          {{-100.3, 16.3}, 32.0, 479, 32.0}};
    // Start moves no scene could tell apart: the single shape's spread over them is its own noise.
    const glm::dvec2 nudges[] = {{0.0, 0.0},  {1e-6, 0.0}, {0.0, 1e-6}, {1e-4, 0.0},
                                 {0.0, 1e-4}, {1e-3, 0.0}, {0.0, 1e-3}};
    constexpr double kLowest = -0.005;
    constexpr double kHighest = 0.0018;
    std::cout.precision(9);
    for (int p = 0; p < 5; ++p)
    {
        const Path &path = paths[p];
        INFO("path " << p);
        int crossed = 0, handed = 0, unowned = 0;
        const Pass pieces = Envelope(TerrainResidencyMode::Interest, path, {0.0, 0.0}, &crossed, &handed, &unowned);
        CHECK(crossed >= 12);
        CHECK(handed > 0);
        CHECK(unowned == 0);

        Pass single;
        Range lows, highs;
        for (const glm::dvec2 nudge : nudges)
        {
            const Pass whole = Envelope(TerrainResidencyMode::Eager, path, nudge);
            if (nudge == glm::dvec2(0.0))
                single = whole;
            lows.Add(whole.lowestGap);
            highs.Add(whole.highestGap);
        }
        // The approved gate where the single shape keeps to it; elsewhere the single shape's noise, widened by itself.
        if (lows.low >= kLowest)
            CHECK(pieces.lowestGap >= kLowest);
        else
            CHECK(pieces.lowestGap >= lows.low - (lows.high - lows.low));
        if (highs.high <= kHighest)
            CHECK(pieces.highestGap <= kHighest);
        else
            CHECK(pieces.highestGap <= highs.high + (highs.high - highs.low));
        if (p == 0)
        {
            CHECK(pieces.lowestGap == single.lowestGap);
            CHECK(pieces.highestGap == single.highestGap);
            CHECK(pieces.kick == single.kick);
        }
        std::cout << "seams,path," << p << ",crossed," << crossed << ",handed," << handed << ",single kick,"
                  << single.kick << ",gap," << single.lowestGap << ',' << single.highestGap << ",noise,"
                  << lows.low << ',' << lows.high << ',' << highs.low << ',' << highs.high << ",pieces kick,"
                  << pieces.kick << ",gap," << pieces.lowestGap << ',' << pieces.highestGap << '\n';
    }
}
