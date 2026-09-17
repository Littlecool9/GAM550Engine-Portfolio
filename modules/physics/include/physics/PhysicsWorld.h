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

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mir
{
    // glm::quat takes (w, x, y, z), so this is the identity rotation. Named
    // because it is the default argument of every body factory below.
    inline const glm::quat kIdentityRotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    // Sizing and tuning fixed at construction - Jolt preallocates from it.
    struct PhysicsWorldDesc
    {
        glm::vec3 mGravity{ 0.0f, -9.81f, 0.0f };

        uint32_t mMaxBodies = 1024;
        uint32_t mMaxBodyPairs = 1024;
        uint32_t mMaxContactConstraints = 1024;

        // 0 lets Jolt pick (hardware concurrency - 1)
        uint32_t mWorkerThreads = 0;

        // Collision steps per Step(); 1 is enough at 60 Hz
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

        // Shape sizes are world space. A Jolt shape cannot be scaled after it
        // is built, so nothing here looks at Transform::Scale().
        BodyHandle CreateStaticBox(const glm::vec3 &position, const glm::vec3 &halfExtent,
                                   const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateStaticSphere(const glm::vec3 &position, float radius,
                                      const glm::quat &rotation = kIdentityRotation);

        // Integrated by every Step(), starts awake.
        BodyHandle CreateDynamicBox(const glm::vec3 &position, const glm::vec3 &halfExtent,
                                    const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateDynamicSphere(const glm::vec3 &position, float radius,
                                       const glm::quat &rotation = kIdentityRotation);

        void DestroyBody(BodyHandle body);
        glm::vec3 BodyPosition(BodyHandle body) const;
        glm::quat BodyRotation(BodyHandle body) const;
        glm::vec3 BodyLinearVelocity(BodyHandle body) const;
        glm::vec3 BodyAngularVelocity(BodyHandle body) const;
        void SetBodyTransform(BodyHandle body, const glm::vec3 &position, const glm::quat &rotation);
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

        // Advances by dt seconds. Non-positive dt is a no-op, not an error: the
        // frame loop can hand us a stalled frame.
        void Step(float dt);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
