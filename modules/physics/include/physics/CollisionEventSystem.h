/**
 * @file CollisionEventSystem.h
 * @author Jackey Cen
 * @brief Publishes the frame's collision events from PhysicsWorld into the CollisionEvents resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/System.h>

namespace mir
{
    class CollisionEventSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override;
    };
}
