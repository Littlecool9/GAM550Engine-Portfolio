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
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <filesystem>
#include <string>

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
    using mir::DiscardSnapshot;
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

    PlaySnapshot snapshot = CaptureWorld(world);

    // "Play" mutates it.
    Transform *transform = world.GetComponent<Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->SetPosition(glm::vec3(99.0f, 99.0f, 99.0f));

    const LoadedScene restored = RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    REQUIRE(restored.mEntities.size() == 1);
    const Entity restoredEntity = restored.mEntities.begin()->second;

    const Transform *restoredTransform = world.GetComponent<Transform>(restoredEntity);
    REQUIRE(restoredTransform != nullptr);
    CHECK(restoredTransform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("RestoreWorld reverts a far root and its child exactly", "[scene]")
{
    mir::RegisterCoreComponents();

    const mir::WorldVec3 rootPosition = mir::WorldVec3(150000.625, -20.0, -75000.25) + mir::WorldVec3(0.001, -0.002, 0.003);
    const mir::WorldVec3 childLocal{0.001, 0.0, -0.002};

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    AddTransform(world, root, Transform{ rootPosition });
    AddTransform(world, child, Transform{ childLocal }, root);
    const mir::WorldVec3 childWorld = mir::ResolveWorld(world, child).position.Meters();

    const PlaySnapshot snapshot = CaptureWorld(world);

    // "Play" moves both by an amount a float at this distance cannot hold
    REQUIRE(world.GetComponent<Transform>(root)->Translate(mir::WorldVec3(0.004)));
    REQUIRE(world.GetComponent<Transform>(child)->Translate(mir::WorldVec3(0.004)));

    const LoadedScene restored = RestoreWorld(world, snapshot);
    REQUIRE(restored.mEntities.size() == 2);

    Entity restoredRoot{};
    Entity restoredChild{};
    for (const auto &[id, entity] : restored.mEntities)
        (mir::ParentOf(world, entity).IsNull() ? restoredRoot : restoredChild) = entity;
    REQUIRE(mir::ParentOf(world, restoredChild) == restoredRoot);

    CHECK(world.GetComponent<Transform>(restoredRoot)->Position() == rootPosition);
    CHECK(world.GetComponent<Transform>(restoredChild)->Position() == childLocal);
    CHECK(mir::ResolveWorld(world, restoredChild).position.Meters() == childWorld);
}

TEST_CASE("RestoreWorld destroys an entity Play spawned that CaptureWorld never saw", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity original = CreateSceneEntity(world, scene);
    AddTransform(world, original);

    PlaySnapshot snapshot = CaptureWorld(world);

    // "Play" spawns something the edit-time scene never tracked.
    Entity spawned = world.CreateEntity();
    AddTransform(world, spawned);
    REQUIRE(world.IsAlive(spawned));

    RestoreWorld(world, snapshot);
    DiscardSnapshot(snapshot);

    CHECK_FALSE(world.IsAlive(spawned));
    CHECK_FALSE(world.IsAlive(original)); // restore gets a new handle, not the old one
}

TEST_CASE("The snapshot's directory is this process's own, and DiscardSnapshot removes it", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    AddTransform(world, CreateSceneEntity(world, scene));

    PlaySnapshot snapshot = CaptureWorld(world);
    const std::filesystem::path dir = snapshot.mDir;
    CHECK(dir.filename().string() == "mjolnir_play_snapshot_" + std::to_string(mir::CurrentProcessId()));
    REQUIRE(std::filesystem::exists(dir));

    DiscardSnapshot(snapshot);
    CHECK_FALSE(std::filesystem::exists(dir));
    CHECK(snapshot.mDir.empty());

    // twice is harmless
    DiscardSnapshot(snapshot);
}
