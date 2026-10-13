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

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace mir
{
    inline const glm::quat kIdentityRotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    // Drops every triangle touching the sample (Jolt's cNoCollisionValue).
    inline constexpr float kHeightFieldHole = std::numeric_limits<float>::max();

    // Cells split along (x, z)-(x+1, z+1), the terrain surface's diagonal.
    struct HeightFieldDesc
    {
        // x fastest.
        std::span<const float> mSamples;
        uint32_t mCountX = 0;
        uint32_t mCountZ = 0;
        float mSpacing = 1.0f;
        // Sample (0, 0) at height 0.
        WorldPosition mOrigin;

        // Uses every stride-th sample; count - 1 must be a multiple of it.
        uint32_t mSampleStride = 1;

        // Jolt's block size; 4 takes ~55% of the memory for ~1.6x the error.
        uint32_t mBlockSize = 2;
        uint32_t mBitsPerSample = 8;

        // Jolt's 16-bit height range; shapes given one range decode a sample they share alike away from
        // their max sides. NaN takes the samples' own range.
        float mQuantizationMin = std::numeric_limits<float>::quiet_NaN();
        float mQuantizationMax = std::numeric_limits<float>::quiet_NaN();
    };

    struct HeightFieldInfo
    {
        // Jolt's square side, padded with holes.
        uint32_t mSampleCount = 0;
        std::size_t mMemoryBytes = 0;
        // Measured from every valid decoded shape vertex, before world placement.
        double mMaxQuantizationError = 0.0;
        double mMaxHorizontalError = 0.0;
        uint64_t mValidSamples = 0;
        double mExtentX = 0.0;
        double mExtentZ = 0.0;
    };

    // A built height field shape that static bodies at any number of places share. Immutable.
    class SharedHeightField
    {
    public:
        bool IsValid() const { return mData != nullptr; }
        // Null for a shape Jolt did not make a height field of.
        const HeightFieldInfo *Info() const;

    private:
        friend class PhysicsWorld;
        friend SharedHeightField BuildHeightField(const HeightFieldDesc &desc);

        struct Data;
        std::shared_ptr<const Data> mData;
    };

    // Any thread; mOrigin is not used. Invalid (logged) on bad input.
    SharedHeightField BuildHeightField(const HeightFieldDesc &desc);

    struct RayHit
    {
        BodyHandle mBody;
        WorldPosition mPoint;
        glm::vec3 mNormal{ 0.0f };
        float mDistance = 0.0f;
    };

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

        // Bodies live in the fixed Scene world coordinate system
        BodyHandle CreateStaticBox(WorldPosition position, const glm::vec3 &halfExtent,
                                   const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateStaticSphere(WorldPosition position, float radius,
                                      const glm::quat &rotation = kIdentityRotation);

        BodyHandle CreateDynamicBox(WorldPosition position, const glm::vec3 &halfExtent,
                                    const glm::quat &rotation = kIdentityRotation);
        BodyHandle CreateDynamicSphere(WorldPosition position, float radius,
                                       const glm::quat &rotation = kIdentityRotation);

        // Builds the shape once; Step never rebuilds it. Invalid handle (logged) on bad input.
        BodyHandle CreateStaticHeightField(const HeightFieldDesc &desc);
        // A static body at origin for a shape built elsewhere; between steps only.
        BodyHandle CreateStaticHeightField(const SharedHeightField &shape, WorldPosition origin);

        // Between steps only. Wakes bodies resting on it.
        void DestroyBody(BodyHandle body);

        // A restricted static body collides only with the dynamic bodies allowed to touch it.
        void RestrictBody(BodyHandle staticBody);
        // The restricted bodies this dynamic body may touch from the next step on; it passes through the others.
        void RestrictContacts(BodyHandle dynamicBody, std::span<const BodyHandle> allowed);
        // Lets the body touch every restricted body again.
        void ClearContactRestriction(BodyHandle dynamicBody);

        // Takes a dynamic body out of the simulation: steps keep its pose and its velocities wait for
        // ResumeBody. While held, velocity changes and impulses land in the kept velocities; forces are dropped.
        void HoldBody(BodyHandle body);
        void ResumeBody(BodyHandle body);
        bool IsBodyHeld(BodyHandle body) const;

        glm::vec3 Gravity() const;

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

        // kNullEntity for an untagged body or a bad handle; the entity is not checked for liveness.
        void SetBodyEntity(BodyHandle body, Entity entity);
        Entity BodyEntity(BodyHandle body) const;

        // Closest hit within maxDistance. Jolt's float fraction resolves ~6e-8 * maxDistance.
        std::optional<RayHit> RayCast(WorldPosition origin, const glm::vec3 &direction, float maxDistance) const;

        // nullopt unless body is a live height field.
        std::optional<HeightFieldInfo> HeightFieldInfoOf(BodyHandle body) const;

        std::size_t HeightFieldsBuilt() const;

        std::size_t BodyCount() const;

        // Advances by dt seconds. Non-positive dt is a no-op, not an error.
        void Step(float dt);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
