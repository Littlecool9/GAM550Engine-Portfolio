/**
 * @file PhysicsStepGate.h
 * @author Jackey Cen
 * @brief Lets another module act on bodies between PhysicsSystem's sync and each fixed step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/World.h>

#include <vector>

namespace mir
{
    class PhysicsWorld;

    class PhysicsStepGate
    {
    public:
        virtual ~PhysicsStepGate() = default;

        // Once per fixed step, after new bodies and Transform edits reach the PhysicsWorld and before it steps.
        virtual void BeforeStep(World &world, PhysicsWorld &physics, float dt) = 0;
    };

    // World resource; a gate removes itself before it goes.
    struct PhysicsStepGates
    {
        std::vector<PhysicsStepGate *> gates;
    };
}
