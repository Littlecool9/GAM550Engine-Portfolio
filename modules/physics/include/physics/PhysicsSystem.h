/**
 * @file PhysicsSystem.h
 * @author Jackey Cen
 * @brief Steps the World's PhysicsWorld and keeps RigidBody and Transform in sync.
 *
 * Registered in SystemPhase::FixedUpdate, so it runs once per fixed substep
 * with context.dt as the step - and before PostUpdate, where
 * TransformPropagateSystem turns the Transforms it wrote into world matrices.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/components/RigidBody.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/Transform.h>

namespace mir
{
    class PhysicsSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

    private:
        static void OnEntityDestroyed(World &world, Entity entity, void *user);

        Query<Transform, RigidBody> *mQuery = nullptr;
    };
}
