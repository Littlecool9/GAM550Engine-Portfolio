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
    // PhysicsSystem creates the body on the first simulating step and keeps
    // Transform and the body in sync from then on. The entity needs a
    // Transform; one without is skipped.
    //
    // The shape sizes below are world space and are NOT multiplied by
    // Transform::Scale(): a Jolt shape cannot be scaled after it is built, so a
    // scaled visual has to have its collider dimensions authored to match.
    struct RigidBody
    {
        // Runtime-only handle
        BodyHandle body;

        // Transform::Version() this component last agreed with. Keep 
        // the transform and physics synced
        uint32_t transformVersion = 0;

        // Below Stored in scene files
        PhysicsMotion motion = PhysicsMotion::Dynamic;
        PhysicsShape shape = PhysicsShape::Box;

        glm::vec3 halfExtent{ 0.5f, 0.5f, 0.5f }; // PhysicsShape::Box
        float radius = 0.5f;                      // PhysicsShape::Sphere
    };

    MIR_ASSERT_COMPONENT(RigidBody);

    inline constexpr FieldDesc kRigidBodyFields[] = {
        {"motion", FieldKind::Int, offsetof(RigidBody, motion)},
        {"shape", FieldKind::Int, offsetof(RigidBody, shape)},
        {"halfExtent", FieldKind::Vec3, offsetof(RigidBody, halfExtent)},
        {"radius", FieldKind::Float, offsetof(RigidBody, radius)},
    };
}
