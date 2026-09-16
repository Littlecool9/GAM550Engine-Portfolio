/**
 * @file System.h
 * @author Sumin Park
 * @brief System interface, execution phases, and the per-frame context
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include <cstdint>

namespace mir
{
    class World;
    class CommandBuffer;

    // Ordering primitive:
    // Each system will execute in the order of assigned phase -> registrated order
    enum class SystemPhase : uint8_t
    {
        PreUpdate,
        Update,
        // Once per fixed substep, 0..N times a frame; context.dt is the fixed step.
        // After Update so physics writes Transform last, before PostUpdate propagates it.
        FixedUpdate,
        PostUpdate,
        Render,
        kCount
    };

    // Everything a system is handed for one tick.
    struct SystemContext
    {
        World &world;
        CommandBuffer &commands;
        float dt = 0.0f;      // seconds since the previous frame; the fixed step in FixedUpdate
        double elapsed = 0.0; // seconds since the first frame
        uint64_t frame = 0;
    };

    // A unit of per-frame work over the World.
    class ISystem
    {
    public:
        virtual ~ISystem() = default;

        ISystem(const ISystem &) = delete;
        ISystem &operator=(const ISystem &) = delete;
        ISystem(ISystem &&) = delete;
        ISystem &operator=(ISystem &&) = delete;

        // Once, before the first OnUpdate. Cache queries here.
        virtual void OnStart(SystemContext &) {}

        // Called on target phase
        virtual void OnUpdate(SystemContext &context) = 0;

        // Once, at teardown. Runs in reverse registration order.
        virtual void OnStop(SystemContext &) {}

    protected:
        ISystem() = default;
    };
}
