/**
 * @file EntityBaseline.cpp
 * @author Sumin Park
 * @brief The components all entity should have: Transform, WorldTransform and Name.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EntityBaseline.h>

#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>

#include <format>

namespace mir
{
    bool IsBaselineComponent(const TypeId &type)
    {
        return type == TypeIdOf<Transform>() || type == TypeIdOf<WorldTransform>() || type == TypeIdOf<Name>();
    }

    void AddMissingBaseline(World &world, Entity entity)
    {
        // AddTransform adds WorldTransform with it
        if (!world.Has<Transform>(entity))
            AddTransform(world, entity);
        if (!world.Has<Name>(entity))
            world.AddComponent<Name>(entity, MakeName(std::format("Entity{}", entity.mIndex)));
    }
}
