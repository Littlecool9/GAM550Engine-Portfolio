/**
 * @file PhysicsSystem.cpp
 * @author Jackey Cen
 * @brief Body lifetime and the Transform <-> RigidBody sync around one step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsSystem.h>

#include <physics/PhysicsWorld.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

namespace mir
{
    namespace
    {
        // A RigidBody belongs on a root, whose Transform position is the
        // absolute world position. The setters keep it finite, so the origin
        // fallback is never taken.
        WorldPosition BodyWorldPosition(const Transform &transform)
        {
            return WorldPosition::FromMeters(transform.Position()).value_or(WorldPosition{});
        }

        BodyHandle CreateBodyFor(PhysicsWorld &physics, const RigidBody &rigidBody, const Transform &transform)
        {
            const bool dynamic = rigidBody.motion == PhysicsMotion::Dynamic;
            const WorldPosition position = BodyWorldPosition(transform);

            if (rigidBody.shape == PhysicsShape::Sphere)
            {
                return dynamic
                    ? physics.CreateDynamicSphere(position, rigidBody.radius, transform.Rotation())
                    : physics.CreateStaticSphere(position, rigidBody.radius, transform.Rotation());
            }

            return dynamic
                ? physics.CreateDynamicBox(position, rigidBody.halfExtent, transform.Rotation())
                : physics.CreateStaticBox(position, rigidBody.halfExtent, transform.Rotation());
        }
    }

    void PhysicsSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, RigidBody>();
        context.world.AddDestroyHook(&PhysicsSystem::OnEntityDestroyed, this);
    }

    void PhysicsSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        PhysicsWorld *physics = context.world.TryResource<PhysicsWorld>();
        if (physics == nullptr)
            return;

        mQuery->ForEach(
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.body.IsValid())
                {
                    // Jolt is world space and Transform is parent space, so a
                    // parented body would drift. Warned once, at creation.
                    if (ParentOf(context.world, entity) != kNullEntity)
                    {
                        MIR_LOG_WARN("PhysicsSystem: entity {} has a parent; its RigidBody is simulated "
                                     "in world space and will not follow the parent",
                            entity.mIndex);
                    }

                    // This runs after ScriptSystem PreUpdate phase,
                    // meaning OnStart() in a script can never find a body.
                    rigidBody.body = CreateBodyFor(*physics, rigidBody, transform);
                }
                else if (transform.Version() != rigidBody.transformVersion)
                {
                    physics->SetBodyTransform(rigidBody.body, BodyWorldPosition(transform), transform.Rotation());
                }

                rigidBody.transformVersion = transform.Version();
            });

        physics->Step(context.dt);

        mQuery->ForEach(
            [&](Entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.body.IsValid() || rigidBody.motion != PhysicsMotion::Dynamic)
                    return;

                // A body Jolt left at a non-finite position keeps its last
                // Transform instead of writing that back.
                const std::optional<WorldPosition> position = physics->BodyPosition(rigidBody.body);
                if (position && position->Meters() != transform.Position())
                    transform.SetPosition(position->Meters());

                const glm::quat rotation = physics->BodyRotation(rigidBody.body);
                if (rotation != transform.Rotation())
                    transform.SetRotation(rotation);

                rigidBody.transformVersion = transform.Version();
            });
    }

    void PhysicsSystem::OnStop(SystemContext &context)
    {
        context.world.RemoveDestroyHook(&PhysicsSystem::OnEntityDestroyed, this);
    }

    void PhysicsSystem::OnEntityDestroyed(World &world, Entity entity, void *)
    {
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        if (rigidBody == nullptr || !rigidBody->body.IsValid())
            return;

        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
            physics->DestroyBody(rigidBody->body);

        rigidBody->body = {};
    }
}
