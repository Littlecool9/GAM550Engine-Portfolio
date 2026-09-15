/**
 * @file PlaySnapshot.tests.cpp
 * @author Sumin Park
 * @brief CaptureWorld/RestoreWorld tests - the same World, mutated and
 *        restored in place, matching how App::PlayScene/StopScene use them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/PlaySnapshot.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

namespace
{
    using mir::AddTransform;
    using mir::CaptureWorld;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlaySnapshot;
    using mir::RestoreWorld;
    using mir::Transform;
    using mir::World;
}

TEST_CASE("RestoreWorld reverts a Transform mutated after CaptureWorld", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    AddTransform(world, entity, Transform{ glm::vec3(1.0f, 2.0f, 3.0f) });

    const PlaySnapshot snapshot = CaptureWorld(world);

    // "Play" mutates it.
    Transform *transform = world.GetComponent<Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->SetPosition(glm::vec3(99.0f, 99.0f, 99.0f));

    const LoadedScene restored = RestoreWorld(world, snapshot);

    REQUIRE(restored.mEntities.size() == 1);
    const Entity restoredEntity = restored.mEntities.begin()->second;

    const Transform *restoredTransform = world.GetComponent<Transform>(restoredEntity);
    REQUIRE(restoredTransform != nullptr);
    CHECK(restoredTransform->Position() == glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("RestoreWorld destroys an entity Play spawned that CaptureWorld never saw", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity original = CreateSceneEntity(world, scene);
    AddTransform(world, original);

    const PlaySnapshot snapshot = CaptureWorld(world);

    // "Play" spawns something the edit-time scene never tracked.
    Entity spawned = world.CreateEntity();
    AddTransform(world, spawned);
    REQUIRE(world.IsAlive(spawned));

    RestoreWorld(world, snapshot);

    CHECK_FALSE(world.IsAlive(spawned));
    CHECK_FALSE(world.IsAlive(original)); // restore gets a new handle, not the old one
}
