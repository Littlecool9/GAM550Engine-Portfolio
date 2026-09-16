/**
 * @file PhysicsTypes.h
 * @author Jackey Cen
 * @brief Handles the physics module hands out.
 *
 * Deliberately free of any Jolt header: Jolt is a PRIVATE dependency of
 * engine_physics, so nothing reachable from include/physics may name it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    // How a body is driven. Static never moves and only collides with things
    // that do; Dynamic is integrated by every Step.
    // int32_t so a component can store it and serialize as FieldKind::Int.
    enum class PhysicsMotion : int32_t
    {
        Static = 0,
        Dynamic = 1,
    };

    // Which member of a shape description is the meaningful one.
    enum class PhysicsShape : int32_t
    {
        Box = 0,
        Sphere = 1,
    };

    // A body owned by one PhysicsWorld.  A handle from
    // one world is meaningless in another.
    struct BodyHandle
    {
        // JPH::BodyID::cInvalidBodyID
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        // The id mirrors Jolt's BodyID value, so 
        // translating it back in PhysicsWorld.cpp is a plain copy
        uint32_t mId = kInvalid;

        bool IsValid() const { return mId != kInvalid; }

        friend bool operator==(BodyHandle, BodyHandle) = default;
    };
}
