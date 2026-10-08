/**
 * @file EntityBaseline.h
 * @author Sumin Park
 * @brief The components all entity should have: Transform, WorldTransform and Name.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <core/ecs/TypeId.h>

namespace mir
{
    class World;

    // the Inspector neither offers these in Add nor lets them be removed
    bool IsBaselineComponent(const TypeId &type);

    // Adds only what is missing, so a preset's own values (its Name) are kept
    void AddMissingBaseline(World &world, Entity entity);
}
