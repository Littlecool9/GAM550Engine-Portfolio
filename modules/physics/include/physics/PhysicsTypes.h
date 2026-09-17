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
    enum class PhysicsMotion : int32_t
    {
        Static = 0,
        Dynamic = 1,
    };

    enum class PhysicsShape : int32_t
    {
        Box = 0,
        Sphere = 1,
    };

    struct BodyHandle
    {
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        uint32_t mId = kInvalid;

        bool IsValid() const { return mId != kInvalid; }

        friend bool operator==(BodyHandle, BodyHandle) = default;
    };
}
