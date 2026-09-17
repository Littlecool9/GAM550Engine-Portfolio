/**
 * @file ComponentRegistration.cpp
 * @author Jackey Cen
 * @brief Registers every component the physics module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/ComponentRegistration.h>

#include <physics/components/RigidBody.h>

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterPhysicsComponents()
    {
        ComponentRegistry::Instance().Register<RigidBody>(kRigidBodyFields);
    }
}
