/**
 * @file PhysicsSystem.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsSystem: body lifetime and Transform sync.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsSystem.h>

#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // The slice of App::Initialize a physics system actually needs.
    struct PhysicsFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };

        explicit PhysicsFixture(mir::EngineMode mode = mir::EngineMode::Play)
        {
            world.EmplaceResource<mir::PhysicsWorld>();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate);
            scheduler.Start(context);
        }

        ~PhysicsFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        // One frame with exactly one fixed substep, as App::Run would drive it.
        void Run(int steps)
        {
            for (int i = 0; i < steps; ++i)
                scheduler.Update(context, 1, kStep);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::Entity Add(const mir::WorldVec3 &position, const mir::RigidBody &rigidBody)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ position });
            world.AddComponent<mir::RigidBody>(entity, rigidBody);
            return entity;
        }

        const mir::Transform &TransformOf(mir::Entity entity)
        {
            return *world.GetComponent<mir::Transform>(entity);
        }
    };

    constexpr mir::RigidBody kDynamicSphere{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Sphere,
        .radius = 0.5f,
    };

    constexpr mir::RigidBody kStaticBox{
        .motion = mir::PhysicsMotion::Static,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(10.0f, 0.5f, 10.0f),
    };
}

TEST_CASE("A dynamic RigidBody drives its Transform", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(30);

    // Half a second of free fall is ~1.2m; a loose bound keeps this off how
    // Jolt spreads gravity across a substep.
    REQUIRE(fixture.TransformOf(entity).Position().y < 4.0f);
    REQUIRE(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("A static RigidBody never moves", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);

    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position() == mir::WorldVec3(0.0));
    REQUIRE(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("Edit mode creates no bodies and moves nothing", "[physics]")
{
    PhysicsFixture fixture(mir::EngineMode::Edit);
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position() == mir::WorldVec3(0.0, 5.0, 0.0));
    REQUIRE(fixture.Physics().BodyCount() == 0);
}

TEST_CASE("Pausing Play stops the simulation", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(30);
    const double fell = fixture.TransformOf(entity).Position().y;

    mir::SetPlayState(fixture.world, mir::PlayState::Paused);
    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position().y == fell);
}

TEST_CASE("A dynamic body lands on a static one", "[physics]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity sphere = fixture.Add(glm::vec3(0.0f, 3.0f, 0.0f), kDynamicSphere);

    fixture.Run(180);

    // Floor top is y = 0.5, so a resting sphere sits one radius above it.
    const double restY = fixture.TransformOf(sphere).Position().y;
    REQUIRE(restY > 0.5f);
    REQUIRE(restY < 1.25f);
}

TEST_CASE("Destroying the entity frees its body", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(1);
    REQUIRE(fixture.Physics().BodyCount() == 1);

    // Same path UnloadScene and PlaySnapshot::RestoreWorld take.
    fixture.world.DestroyEntity(entity);
    REQUIRE(fixture.Physics().BodyCount() == 0);
}

TEST_CASE("Writing the Transform teleports the body", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(10);
    REQUIRE(fixture.TransformOf(entity).Position().y < 5.0f);

    // What a script that assigns Transform.position looks like from here.
    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(glm::vec3(3.0f, 10.0f, 0.0f));
    fixture.Run(1);

    const mir::WorldVec3 position = fixture.TransformOf(entity).Position();
    REQUIRE(std::abs(position.x - 3.0) < 0.01);
    REQUIRE(position.y > 9.9f);
}

namespace
{
    // Map-edge coordinates shared with the terrain ADRs.
    const mir::WorldVec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const mir::WorldVec3 kEdgeB{150000.625, -20.0, -75000.25};
    const mir::WorldVec3 kNudge{0.001, -0.002, 0.003};

    mir::WorldVec3 BodyMeters(PhysicsFixture &fixture, mir::Entity entity)
    {
        const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
        REQUIRE(rigidBody != nullptr);
        const std::optional<mir::WorldPosition> position = fixture.Physics().BodyPosition(rigidBody->body);
        REQUIRE(position);
        return position->Meters();
    }
}

TEST_CASE("A far Transform reaches its body without losing a millimeter", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    PhysicsFixture fixture;
    const mir::Entity box = fixture.Add(kEdgeB + kNudge, kStaticBox);

    fixture.Run(1);
    CHECK(BodyMeters(fixture, box) == kEdgeB + kNudge);
    CHECK(fixture.TransformOf(box).Position() == kEdgeB + kNudge);

    // an outside edit of another millimeter is pushed to the body as is
    REQUIRE(fixture.world.GetComponent<mir::Transform>(box)->Translate(kNudge));
    fixture.Run(1);
    const mir::WorldVec3 moved = (kEdgeB + kNudge) + kNudge;
    CHECK(BodyMeters(fixture, box) == moved);
    CHECK(fixture.TransformOf(box).Position() == moved);
}

TEST_CASE("A far dynamic body writes back what Jolt simulated", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double fell[3];
    const mir::WorldVec3 starts[3] = {mir::WorldVec3(0.0, 5.0, 0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        PhysicsFixture fixture;
        const mir::Entity sphere = fixture.Add(starts[i] + kNudge, kDynamicSphere);

        fixture.Run(30);

        const mir::WorldVec3 position = fixture.TransformOf(sphere).Position();
        CHECK(position == BodyMeters(fixture, sphere));
        // free fall is straight down: the sideways millimeters are untouched
        CHECK(position.x == starts[i].x + kNudge.x);
        CHECK(position.z == starts[i].z + kNudge.z);
        fell[i] = position.y - starts[i].y;
    }

    CHECK(fell[0] < -1.0);
    CHECK(std::abs(fell[1] - fell[0]) <= 1e-9);
    CHECK(std::abs(fell[2] - fell[0]) <= 1e-9);
}

TEST_CASE("A far dynamic body lands on a far static one", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double restHeight[2];
    const mir::WorldVec3 floors[2] = {mir::WorldVec3(0.0), kEdgeA};
    for (int i = 0; i < 2; ++i)
    {
        PhysicsFixture fixture;
        fixture.Add(floors[i], kStaticBox);
        const mir::Entity sphere = fixture.Add(floors[i] + mir::WorldVec3(0.0, 3.0, 0.0), kDynamicSphere);

        fixture.Run(180);

        restHeight[i] = fixture.TransformOf(sphere).Position().y - floors[i].y;
        CHECK(restHeight[i] > 0.5);
        CHECK(restHeight[i] < 1.25);
    }

    CHECK(std::abs(restHeight[1] - restHeight[0]) <= 1e-4);
}
