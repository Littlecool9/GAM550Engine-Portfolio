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

#include <cmath>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    void StepFor(mir::PhysicsWorld &world, int steps)
    {
        for (int i = 0; i < steps; ++i)
            world.Step(kStep);
    }

    // A wide, thin slab centred on the origin; its top surface is at y = 0.5.
    mir::BodyHandle AddFloor(mir::PhysicsWorld &world)
    {
        return world.CreateStaticBox(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f));
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
    REQUIRE(world.BodyPosition(floor).y == 0.0f);
}

TEST_CASE("A dynamic sphere falls under gravity", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateDynamicSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f);
    REQUIRE(sphere.IsValid());

    StepFor(world, 30);

    // Half a second of 9.81 m/s^2 is ~1.2m; a loose bound keeps this from
    // depending on how Jolt distributes gravity across substeps.
    REQUIRE(world.BodyPosition(sphere).y < 4.0f);
    REQUIRE(world.BodyLinearVelocity(sphere).y < 0.0f);
}

TEST_CASE("A dynamic sphere comes to rest on a static floor", "[physics]")
{
    mir::PhysicsWorld world;

    AddFloor(world);
    const mir::BodyHandle sphere = world.CreateDynamicSphere(glm::vec3(0.0f, 3.0f, 0.0f), 0.5f);
    REQUIRE(world.BodyCount() == 2);

    StepFor(world, 180);

    // Resting means the centre sits one radius above the floor top (y = 1.0).
    const float restY = world.BodyPosition(sphere).y;
    REQUIRE(restY > 0.5f);  // did not tunnel through
    REQUIRE(restY < 1.25f); // actually landed instead of hanging in the air
}

TEST_CASE("Gravity is taken from the description", "[physics]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f, 0.0f, 0.0f);

    mir::PhysicsWorld world(desc);

    const mir::BodyHandle sphere = world.CreateDynamicSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f);
    StepFor(world, 30);

    REQUIRE(std::abs(world.BodyPosition(sphere).y - 5.0f) < 0.01f);
}

TEST_CASE("DestroyBody drops the body", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const mir::BodyHandle sphere = world.CreateDynamicSphere(glm::vec3(0.0f, 3.0f, 0.0f), 0.5f);
    REQUIRE(world.BodyCount() == 2);

    world.DestroyBody(sphere);
    REQUIRE(world.BodyCount() == 1);

    // Destroying twice, and destroying an invalid handle, are both no-ops.
    world.DestroyBody(sphere);
    world.DestroyBody(mir::BodyHandle{});
    REQUIRE(world.BodyCount() == 1);

    world.Step(kStep);
    REQUIRE(world.BodyPosition(floor).y == 0.0f);
}

TEST_CASE("A bad handle reads as zero instead of crashing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle invalid;
    REQUIRE_FALSE(invalid.IsValid());
    REQUIRE(world.BodyPosition(invalid) == glm::vec3(0.0f));
    REQUIRE(world.BodyLinearVelocity(invalid) == glm::vec3(0.0f));

    const mir::BodyHandle destroyed = world.CreateDynamicSphere(glm::vec3(0.0f, 3.0f, 0.0f), 0.5f);
    world.DestroyBody(destroyed);
    REQUIRE(world.BodyPosition(destroyed) == glm::vec3(0.0f));
}

TEST_CASE("Degenerate shapes are rejected, not asserted on", "[physics]")
{
    mir::PhysicsWorld world;

    REQUIRE_FALSE(world.CreateStaticSphere(glm::vec3(0.0f), 0.0f).IsValid());
    REQUIRE_FALSE(world.CreateDynamicSphere(glm::vec3(0.0f), -1.0f).IsValid());
    REQUIRE_FALSE(world.CreateStaticBox(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 1.0f)).IsValid());
    REQUIRE(world.BodyCount() == 0);
}

TEST_CASE("A non-positive step does nothing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateDynamicSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f);

    world.Step(0.0f);
    world.Step(-1.0f);

    REQUIRE(world.BodyPosition(sphere).y == 5.0f);
    REQUIRE(world.BodyLinearVelocity(sphere).y == 0.0f);
}

TEST_CASE("Several worlds can exist at once", "[physics]")
{
    {
        mir::PhysicsWorld first;
        mir::PhysicsWorld second;

        const mir::BodyHandle a = first.CreateDynamicSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f);
        const mir::BodyHandle b = second.CreateStaticSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f);

        StepFor(first, 30);
        StepFor(second, 30);

        REQUIRE(first.BodyCount() == 1);
        REQUIRE(second.BodyCount() == 1);
        REQUIRE(first.BodyPosition(a).y < 5.0f);
        REQUIRE(second.BodyPosition(b).y == 5.0f);

        // A handle only means something to the world that issued it.
        REQUIRE(second.BodyCount() == 1);
    }

    // Both refs are gone, so the Jolt globals were torn down; building another
    // world has to bring them back up.
    mir::PhysicsWorld third;
    REQUIRE(third.CreateDynamicSphere(glm::vec3(0.0f, 5.0f, 0.0f), 0.5f).IsValid());
    third.Step(kStep);
}
