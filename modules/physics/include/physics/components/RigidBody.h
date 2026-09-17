/**
 * @file RigidBody.h
 * @author Jackey Cen
 * @brief Gives an entity a body in the World's PhysicsWorld.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace mir
{
    struct RigidBody
    {
        BodyHandle body;

        uint32_t transformVersion = 0;

        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;

        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f };
        float radius = 0.5f;
    };

    MIR_ASSERT_COMPONENT(RigidBody);

    inline constexpr FieldDesc kRigidBodyFields[] = {
        {"motion", FieldKind::Int, offsetof(RigidBody, motion)},
        {"shape", FieldKind::Int, offsetof(RigidBody, shape)},
        {"halfExtent", FieldKind::Vec3, offsetof(RigidBody, halfExtent)},
        {"radius", FieldKind::Float, offsetof(RigidBody, radius)},
    };
}
