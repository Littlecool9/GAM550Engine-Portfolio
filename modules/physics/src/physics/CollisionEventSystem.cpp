/**
 * @file CollisionEventSystem.cpp
 * @author Jackey Cen
 * @brief Moves PhysicsWorld's drained collision events into CollisionEvents once per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/CollisionEventSystem.h>

#include <physics/CollisionEvents.h>
#include <physics/PhysicsWorld.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

#include <utility>
#include <vector>

namespace mir
{
    void CollisionEventSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;

        // Held while paused so scripts still see the list on resume.
        if (IsPaused(world))
            return;

        std::vector<CollisionEvent> drained;
        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
            drained = physics->DrainCollisionEvents();

        if (!IsSimulating(world))
            drained.clear();

        if (CollisionEvents *events = world.TryResource<CollisionEvents>(); events != nullptr)
            events->mEvents = std::move(drained);
    }
}
