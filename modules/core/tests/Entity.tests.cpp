/**
 * @file Entity.tests.cpp
 * @author Sumin Park
 * @brief Tests for the entity handle and its scripting encoding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/Entity.h>

#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
    using mir::Entity;
    using mir::PackEntity;
    using mir::UnpackEntity;
    using mir::World;
}

TEST_CASE("Packing an entity round-trips both halves")
{
    const Entity entity{7, 3};

    CHECK(UnpackEntity(PackEntity(entity)) == entity);
    CHECK(UnpackEntity(PackEntity(mir::kNullEntity)) == mir::kNullEntity);
}

TEST_CASE("Packing keeps the generation that a truncated encoding would lose")
{
    // The failure this guards against: an encoding that drops the high half
    // makes a recycled slot's old handle compare equal to the new one, and the
    // ABA guard stops working.
    World world;

    const Entity first = world.CreateEntity();
    world.DestroyEntity(first);
    const Entity recycled = world.CreateEntity();

    REQUIRE(first.mIndex == recycled.mIndex);
    REQUIRE(first.mGeneration != recycled.mGeneration);

    CHECK(PackEntity(first) != PackEntity(recycled));
    CHECK_FALSE(world.IsAlive(UnpackEntity(PackEntity(first))));
    CHECK(world.IsAlive(UnpackEntity(PackEntity(recycled))));
}

TEST_CASE("A free slot's next-generation handle is not alive")
{
    // ForEachAliveEntity builds handles from GenerationOf; a free slot's must not pass
    World world;

    const Entity destroyed = world.CreateEntity();
    world.DestroyEntity(destroyed);

    const Entity neverIssued{ destroyed.mIndex, destroyed.mGeneration + 1 };
    CHECK_FALSE(world.IsAlive(neverIssued));
}

TEST_CASE("ForEachAliveEntity skips destroyed entities")
{
    World world;

    const Entity kept = world.CreateEntity();
    world.DestroyEntity(world.CreateEntity());

    int visited = 0;
    world.ForEachAliveEntity(
        [&](Entity entity)
        {
            ++visited;
            CHECK(entity == kept);
        });
    CHECK(visited == 1);
}

TEST_CASE("A packed entity uses the full 64 bits")
{
    // Which is why it must not travel through a VM's double: 53 bits of
    // mantissa would silently truncate this one.
    const Entity extreme{0xFFFFFFFEu, 0xFFFFFFFFu};
    const uint64_t packed = PackEntity(extreme);

    CHECK(packed > (1ull << 53));
    CHECK(UnpackEntity(packed) == extreme);
}
