/**
 * @file JoltRuntime.h
 * @author Jackey Cen
 * @brief Process-wide Jolt setup, reference counted.
 *
 * Jolt's allocator, Factory and type registry are globals, so they must be
 * installed exactly once however many PhysicsWorlds exist. Every world holds a
 * JoltRuntimeRef; the last one to go tears the globals back down.
 *
 * Internal to engine_physics: it names Jolt, so it stays out of include/.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir::detail
{
    class JoltRuntimeRef
    {
    public:
        JoltRuntimeRef();
        ~JoltRuntimeRef();

        JoltRuntimeRef(const JoltRuntimeRef &) = delete;
        JoltRuntimeRef &operator=(const JoltRuntimeRef &) = delete;
        JoltRuntimeRef(JoltRuntimeRef &&) = delete;
        JoltRuntimeRef &operator=(JoltRuntimeRef &&) = delete;
    };
}
