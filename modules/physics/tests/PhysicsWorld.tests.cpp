/**
 * @file PhysicsWorld.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsWorld.
 *
 * These also stand in for a Jolt integration test: they are the only place that
 * proves the globals in JoltRuntime.cpp are installed and torn down correctly.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Map-edge coordinates shared with the terrain ADRs.
    const mir::WorldVec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const mir::WorldVec3 kEdgeB{150000.625, -20.0, -75000.25};
    const mir::WorldVec3 kNudge{0.001, -0.002, 0.003};

    mir::WorldPosition At(const mir::WorldVec3 &meters)
    {
        const std::optional<mir::WorldPosition> position = mir::WorldPosition::FromMeters(meters);
        REQUIRE(position);
        return *position;
    }

    mir::WorldPosition At(mir::WorldReal x, mir::WorldReal y, mir::WorldReal z) { return At(mir::WorldVec3(x, y, z)); }

    mir::WorldVec3 MetersOf(const mir::PhysicsWorld &world, mir::BodyHandle body)
    {
        const std::optional<mir::WorldPosition> position = world.BodyPosition(body);
        REQUIRE(position);
        return position->Meters();
    }

    double MaxError(const mir::WorldVec3 &actual, const mir::WorldVec3 &expected)
    {
        const mir::WorldVec3 error = glm::abs(actual - expected);
        return std::max({error.x, error.y, error.z});
    }

    void StepFor(mir::PhysicsWorld &world, int steps)
    {
        for (int i = 0; i < steps; ++i)
            world.Step(kStep);
    }

    // A wide, thin slab centred on `center`; its top surface is 0.5 above it.
    mir::BodyHandle AddFloor(mir::PhysicsWorld &world, const mir::WorldVec3 &center = mir::WorldVec3(0.0))
    {
        return world.CreateStaticBox(At(center), glm::vec3(10.0f, 0.5f, 10.0f));
    }
}

TEST_CASE("A world with a static body takes a step", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    REQUIRE(floor.IsValid());
    REQUIRE(world.BodyCount() == 1);

    world.Step(kStep);

    REQUIRE(world.BodyCount() == 1);
    REQUIRE(MetersOf(world, floor).y == 0.0);
}

TEST_CASE("A dynamic sphere falls under gravity", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateDynamicSphere(At(0.0, 5.0, 0.0), 0.5f);
    REQUIRE(sphere.IsValid());

    StepFor(world, 30);

    // Half a second of 9.81 m/s^2 is ~1.2m; a loose bound keeps this from
    // depending on how Jolt distributes gravity across substeps.
    REQUIRE(MetersOf(world, sphere).y < 4.0);
    REQUIRE(world.BodyLinearVelocity(sphere).y < 0.0f);
}

TEST_CASE("A dynamic sphere comes to rest on a static floor", "[physics]")
{
    mir::PhysicsWorld world;

    AddFloor(world);
    const mir::BodyHandle sphere = world.CreateDynamicSphere(At(0.0, 3.0, 0.0), 0.5f);
    REQUIRE(world.BodyCount() == 2);

    StepFor(world, 180);

    // Resting means the centre sits one radius above the floor top (y = 1.0).
    const double restY = MetersOf(world, sphere).y;
    REQUIRE(restY > 0.5);  // did not tunnel through
    REQUIRE(restY < 1.25); // actually landed instead of hanging in the air
}

TEST_CASE("Gravity is taken from the description", "[physics]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f, 0.0f, 0.0f);

    mir::PhysicsWorld world(desc);

    const mir::BodyHandle sphere = world.CreateDynamicSphere(At(0.0, 5.0, 0.0), 0.5f);
    StepFor(world, 30);

    REQUIRE(std::abs(MetersOf(world, sphere).y - 5.0) < 0.01);
}

TEST_CASE("DestroyBody drops the body", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const mir::BodyHandle sphere = world.CreateDynamicSphere(At(0.0, 3.0, 0.0), 0.5f);
    REQUIRE(world.BodyCount() == 2);

    world.DestroyBody(sphere);
    REQUIRE(world.BodyCount() == 1);

    // Destroying twice, and destroying an invalid handle, are both no-ops.
    world.DestroyBody(sphere);
    world.DestroyBody(mir::BodyHandle{});
    REQUIRE(world.BodyCount() == 1);

    world.Step(kStep);
    REQUIRE(MetersOf(world, floor).y == 0.0);
}

TEST_CASE("A bad handle has no position instead of crashing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle invalid;
    REQUIRE_FALSE(invalid.IsValid());
    REQUIRE_FALSE(world.BodyPosition(invalid));
    REQUIRE(world.BodyLinearVelocity(invalid) == glm::vec3(0.0f));

    // the Scene origin is a real place, so a body that is gone must not report it
    const mir::BodyHandle destroyed = world.CreateDynamicSphere(At(0.0, 3.0, 0.0), 0.5f);
    world.DestroyBody(destroyed);
    REQUIRE_FALSE(world.BodyPosition(destroyed));
}

TEST_CASE("Degenerate shapes are rejected, not asserted on", "[physics]")
{
    mir::PhysicsWorld world;

    REQUIRE_FALSE(world.CreateStaticSphere(mir::WorldPosition{}, 0.0f).IsValid());
    REQUIRE_FALSE(world.CreateDynamicSphere(mir::WorldPosition{}, -1.0f).IsValid());
    REQUIRE_FALSE(world.CreateStaticBox(mir::WorldPosition{}, glm::vec3(1.0f, 0.0f, 1.0f)).IsValid());
    REQUIRE(world.BodyCount() == 0);
}

TEST_CASE("A non-positive step does nothing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateDynamicSphere(At(0.0, 5.0, 0.0), 0.5f);

    world.Step(0.0f);
    world.Step(-1.0f);

    REQUIRE(MetersOf(world, sphere).y == 5.0);
    REQUIRE(world.BodyLinearVelocity(sphere).y == 0.0f);
}

TEST_CASE("Several worlds can exist at once", "[physics]")
{
    {
        mir::PhysicsWorld first;
        mir::PhysicsWorld second;

        const mir::BodyHandle a = first.CreateDynamicSphere(At(0.0, 5.0, 0.0), 0.5f);
        const mir::BodyHandle b = second.CreateStaticSphere(At(0.0, 5.0, 0.0), 0.5f);

        StepFor(first, 30);
        StepFor(second, 30);

        REQUIRE(first.BodyCount() == 1);
        REQUIRE(second.BodyCount() == 1);
        REQUIRE(MetersOf(first, a).y < 5.0);
        REQUIRE(MetersOf(second, b).y == 5.0);

        // A handle only means something to the world that issued it.
        REQUIRE(second.BodyCount() == 1);
    }

    // Both refs are gone, so the Jolt globals were torn down; building another
    // world has to bring them back up.
    mir::PhysicsWorld third;
    REQUIRE(third.CreateDynamicSphere(At(0.0, 5.0, 0.0), 0.5f).IsValid());
    third.Step(kStep);
}

TEST_CASE("A body keeps a millimeter at the map edge", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    for (const mir::WorldVec3 &edge : {kEdgeA, kEdgeB})
    {
        mir::PhysicsWorld world;

        // fails with a float Jolt, or with a float cast anywhere on the way in or out
        const mir::BodyHandle body = world.CreateStaticSphere(At(edge + kNudge), 0.5f);
        CHECK(MetersOf(world, body) == edge + kNudge);

        world.SetBodyTransform(body, At(edge + kNudge + kNudge), mir::kIdentityRotation);
        CHECK(MetersOf(world, body) == edge + kNudge + kNudge);

        world.Step(kStep);
        CHECK(MetersOf(world, body) == edge + kNudge + kNudge);

        // as a float world position the nudge is gone
        CHECK(static_cast<float>(edge.x + kNudge.x) == static_cast<float>(edge.x));
    }
}

TEST_CASE("A slow body at the map edge moves as it does at the origin", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // 0.05 m/s is 0.83 mm a step, far below the 1/64 m spacing of a float out here
    mir::WorldVec3 travelled[3];
    const mir::WorldVec3 starts[3] = {mir::WorldVec3(0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        mir::PhysicsWorldDesc desc;
        desc.mGravity = glm::vec3(0.0f);
        mir::PhysicsWorld world(desc);

        const mir::BodyHandle sphere = world.CreateDynamicSphere(At(starts[i]), 0.5f);
        world.SetBodyLinearVelocity(sphere, glm::vec3(0.05f, 0.0f, -0.025f));
        StepFor(world, 20);

        travelled[i] = MetersOf(world, sphere) - starts[i];
    }

    CHECK(travelled[0].x > 0.015);
    CHECK(travelled[0].z < -0.0075);
    CHECK(MaxError(travelled[1], travelled[0]) <= 1e-9);
    CHECK(MaxError(travelled[2], travelled[0]) <= 1e-9);
}

TEST_CASE("Bodies collide at the map edge as they do at the origin", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double restHeight[3];
    const mir::WorldVec3 floors[3] = {mir::WorldVec3(0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        mir::PhysicsWorld world;

        AddFloor(world, floors[i]);
        const mir::BodyHandle sphere = world.CreateDynamicSphere(At(floors[i] + mir::WorldVec3(0.0, 3.0, 0.0)), 0.5f);
        StepFor(world, 180);

        const mir::WorldVec3 rest = MetersOf(world, sphere) - floors[i];
        restHeight[i] = rest.y;

        // landed on the floor, and did not drift sideways
        CHECK(rest.y > 0.5);
        CHECK(rest.y < 1.25);
        CHECK(std::abs(rest.x) <= 1e-3);
        CHECK(std::abs(rest.z) <= 1e-3);
    }

    CHECK(std::abs(restHeight[1] - restHeight[0]) <= 1e-4);
    CHECK(std::abs(restHeight[2] - restHeight[0]) <= 1e-4);
}
