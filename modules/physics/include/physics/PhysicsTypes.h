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
#include <string_view>

namespace mir
{
    enum class PhysicsMotion : int32_t
    {
        Static = 0,
        Dynamic = 1,
    };

    // Index == underlying PhysicsMotion value; for RigidBody's "motion" inspector field.
    inline constexpr std::string_view kPhysicsMotionNames[] = { "Static", "Dynamic" };

    enum class PhysicsShape : int32_t
    {
        Box = 0,
        Sphere = 1,
    };

    // Index == underlying PhysicsShape value; for RigidBody's "shape" inspector field.
    inline constexpr std::string_view kPhysicsShapeNames[] = { "Box", "Sphere" };

    struct BodyHandle
    {
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        uint32_t mId = kInvalid;

        bool IsValid() const { return mId != kInvalid; }

        friend bool operator==(BodyHandle, BodyHandle) = default;
    };
}
