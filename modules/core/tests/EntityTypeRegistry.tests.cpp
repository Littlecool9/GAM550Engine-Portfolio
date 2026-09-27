/**
 * @file EntityTypeRegistry.tests.cpp
 * @author Rahul Nair
 * @brief Tests for the editor create-menu's name -> entity factory registry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/EntityTypeRegistry.h>

#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using mir::Entity;
    using mir::EntityTypeDesc;
    using mir::EntityTypeRegistry;
    using mir::Transform;
    using mir::World;

    // Distinct from every other test's types: the registry is process-wide, so a name registered here is visible to the whole binary.
    struct RegistryProbe
    {
    };
}

TEST_CASE("EntityTypeRegistry: registered type appears in All() and its factory runs", "[core][ecs]")
{
    EntityTypeRegistry::Instance().Register("EntityTypeRegistryTestProbe", [](World &world, Entity entity)
        {
            world.AddComponent<Transform>(entity, Transform{});
            world.AddTag<RegistryProbe>(entity);
        });

    const EntityTypeDesc *found = nullptr;
    for (const EntityTypeDesc &type : EntityTypeRegistry::Instance().All())
        if (type.mName == "EntityTypeRegistryTestProbe")
            found = &type;

    REQUIRE(found != nullptr);

    World world;
    const Entity entity = world.CreateEntity();
    found->mCreate(world, entity);

    CHECK(world.Has<Transform>(entity));
    CHECK(world.Has<RegistryProbe>(entity));
}
