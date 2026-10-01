/**
 * @file PhysicsWorld.h
 * @author Jackey Cen
 * @brief A rigid body simulation: create bodies, then Step() it.
 *
 * Jolt lives entirely behind the pimpl. Several worlds can exist at once; the
 * process-wide Jolt globals behind them are reference counted (JoltRuntime.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace mir
{
    inline const glm::quat kIdentityRotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    struct PhysicsWorldDesc
    {
        glm::vec3 mGravity{ 0.0f, -9.81f, 0.0f };

        uint32_t mMaxBodies = 1024;
        uint32_t mMaxBodyPairs = 1024;
        uint32_t mMaxContactConstraints = 1024;

        uint32_t mWorkerThreads = 0;

        int mCollisionSteps = 1;
    };

    class PhysicsWorld
    {
    public:
        explicit PhysicsWorld(const PhysicsWorldDesc &desc = {});
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld &) = delete;
        PhysicsWorld &operator=(const PhysicsWorld &) = delete;
        PhysicsWorld(PhysicsWorld &&) = delete;
        PhysicsWorld &operator=(PhysicsWorld &&) = delete;

        // Bodies live in the fixed Scene world: a position is a WorldPosition
        // all the way into Jolt (JPH::Real matches WorldReal), never a
        // camera-relative one. Shape sizes, rotations, velocities and forces
        // are bounded floats.
        BodyHandle CreateStaticBox(WorldPosition position, const glm::vec3 &halfExtent,
                                   const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateStaticSphere(WorldPosition position, float radius,
                                      const glm::quat &rotation = kIdentityRotation);

        BodyHandle CreateDynamicBox(WorldPosition position, const glm::vec3 &halfExtent,
                                    const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateDynamicSphere(WorldPosition position, float radius,
                                       const glm::quat &rotation = kIdentityRotation);

        void DestroyBody(BodyHandle body);

        // nullopt for an invalid handle, or if the simulation left the body at
        // a non-finite position; there is no default place to report instead.
        std::optional<WorldPosition> BodyPosition(BodyHandle body) const;
        glm::quat BodyRotation(BodyHandle body) const;
        glm::vec3 BodyLinearVelocity(BodyHandle body) const;
        glm::vec3 BodyAngularVelocity(BodyHandle body) const;
        void SetBodyTransform(BodyHandle body, WorldPosition position, const glm::quat &rotation);
        void SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        void SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity);
        void AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity);
        void AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity);

        // Force and torque accumulate until the next Step() consumes and clears
        // them, so a continuous push has to be re-applied every step. 
        void AddForce(BodyHandle body, const glm::vec3 &force);
        void AddTorque(BodyHandle body, const glm::vec3 &torque);
        void AddImpulse(BodyHandle body, const glm::vec3 &impulse);
        void AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse);
        void AddForceAtOffset(BodyHandle body, const glm::vec3 &force, const glm::vec3 &localOffset);
        void AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse, const glm::vec3 &localOffset);

        std::size_t BodyCount() const;

        // Advances by dt seconds. Non-positive dt is a no-op, not an error.
        void Step(float dt);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
