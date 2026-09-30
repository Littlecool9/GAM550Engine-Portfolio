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

        mir::Entity Add(const glm::vec3 &position, const mir::RigidBody &rigidBody)
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

    REQUIRE(fixture.TransformOf(entity).Position() == glm::dvec3(0.0));
    REQUIRE(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("Edit mode creates no bodies and moves nothing", "[physics]")
{
    PhysicsFixture fixture(mir::EngineMode::Edit);
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position() == glm::dvec3(0.0, 5.0, 0.0));
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

    const glm::dvec3 position = fixture.TransformOf(entity).Position();
    REQUIRE(std::abs(position.x - 3.0) < 0.01);
    REQUIRE(position.y > 9.9f);
}
