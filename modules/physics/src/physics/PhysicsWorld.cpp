/**
 * @file PhysicsWorld.cpp
 * @author Jackey Cen
 * @brief The Jolt side of PhysicsWorld: layers, filters, bodies and the step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>

#include "JoltRuntime.h"

#include <core/log/Log.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <optional>
#include <utility>

namespace
{
    // Two layers is the minimum that still lets the broad phase skip the
    // static-vs-static half of the world.
    namespace Layers
    {
        constexpr JPH::ObjectLayer kNonMoving = 0;
        constexpr JPH::ObjectLayer kMoving = 1;
        constexpr JPH::ObjectLayer kCount = 2;
    }

    namespace BroadPhaseLayers
    {
        constexpr JPH::BroadPhaseLayer kNonMoving(0);
        constexpr JPH::BroadPhaseLayer kMoving(1);
        constexpr JPH::uint kCount = 2;
    }

    // Statics never collide with statics; every other pair does.
    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            return a == Layers::kMoving || b == Layers::kMoving;
        }
    };

    class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BroadPhaseLayerInterfaceImpl()
        {
            mLayers[Layers::kNonMoving] = BroadPhaseLayers::kNonMoving;
            mLayers[Layers::kMoving] = BroadPhaseLayers::kMoving;
        }

        JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            JPH_ASSERT(layer < Layers::kCount);
            return mLayers[layer];
        }

    #if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
        {
            return layer == BroadPhaseLayers::kMoving ? "MOVING" : "NON_MOVING";
        }
    #endif

    private:
        JPH::BroadPhaseLayer mLayers[Layers::kCount];
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
        {
            return layer == Layers::kMoving || broadPhase == BroadPhaseLayers::kMoving;
        }
    };

    JPH::Vec3 ToJolt(const glm::vec3 &v)
    {
        return JPH::Vec3(v.x, v.y, v.z);
    }

    JPH::Quat ToJolt(const glm::quat &q)
    {
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    glm::quat FromJolt(JPH::QuatArg q)
    {
        return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }

    glm::vec3 FromJolt(JPH::Vec3Arg v)
    {
        return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    // A float Jolt would round every world position on the way in. CMake and
    // JoltRuntime check the package; this checks what this file compiled against.
    static_assert(sizeof(JPH::Real) == sizeof(double),
        "PhysicsWorld needs Jolt built with JPH_DOUBLE_PRECISION (vcpkg-overlays/joltphysics)");

    JPH::RVec3 ToJolt(mir::WorldPosition position)
    {
        const glm::dvec3 meters = position.Meters();
        return JPH::RVec3(meters.x, meters.y, meters.z);
    }

    std::optional<mir::WorldPosition> PositionFromJolt(JPH::RVec3Arg v)
    {
        return mir::WorldPosition::FromMeters(glm::dvec3(v.GetX(), v.GetY(), v.GetZ()));
    }

    JPH::BodyID ToBodyId(mir::BodyHandle handle)
    {
        return JPH::BodyID(handle.mId);
    }

    JPH::RVec3 OffsetToWorldPoint(const JPH::BodyInterface &bodies, JPH::BodyID id,
        const glm::vec3 &localOffset)
    {
        const JPH::Vec3 worldOffset = bodies.GetRotation(id) * ToJolt(localOffset);
        return bodies.GetCenterOfMassPosition(id) + worldOffset;
    }
}

namespace mir
{
    struct PhysicsWorld::Impl
    {
        explicit Impl(const PhysicsWorldDesc &desc)
            : mJobSystem(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs),
                  static_cast<JPH::uint>(JPH::cMaxPhysicsBarriers),
                  desc.mWorkerThreads > 0 ? static_cast<int>(desc.mWorkerThreads) : -1)
            , mCollisionSteps(desc.mCollisionSteps > 0 ? desc.mCollisionSteps : 1)
        {
            mSystem.Init(desc.mMaxBodies,
                0, // body mutexes: 0 lets Jolt pick a default
                desc.mMaxBodyPairs,
                desc.mMaxContactConstraints,
                mBroadPhaseLayers,
                mObjectVsBroadPhase,
                mObjectLayerPairs);

            mSystem.SetGravity(ToJolt(desc.mGravity));
        }

        BodyHandle CreateBody(const JPH::Shape *shape, WorldPosition position,
            const glm::quat &rotation, bool dynamic)
        {
            const JPH::RefConst<JPH::Shape> owned(shape);

            const JPH::BodyCreationSettings settings(owned,
                ToJolt(position),
                ToJolt(rotation),
                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                dynamic ? Layers::kMoving : Layers::kNonMoving);

            const JPH::BodyID id = mSystem.GetBodyInterface().CreateAndAddBody(settings,
                dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

            if (id.IsInvalid())
            {
                MIR_LOG_ERROR("PhysicsWorld: out of bodies, raise PhysicsWorldDesc::mMaxBodies");
                return {};
            }

            if (!dynamic)
                mBroadPhaseDirty = true;

            return BodyHandle{ id.GetIndexAndSequenceNumber() };
        }

        // the first in declaration order to outlive other systems
        detail::JoltRuntimeRef mRuntime;

        JPH::TempAllocatorImpl mTempAllocator{ 10 * 1024 * 1024 };
        JPH::JobSystemThreadPool mJobSystem;

        BroadPhaseLayerInterfaceImpl mBroadPhaseLayers;
        ObjectVsBroadPhaseLayerFilterImpl mObjectVsBroadPhase;
        ObjectLayerPairFilterImpl mObjectLayerPairs;

        JPH::PhysicsSystem mSystem;

        int mCollisionSteps = 1;
        bool mBroadPhaseDirty = false;
    };

    PhysicsWorld::PhysicsWorld(const PhysicsWorldDesc &desc)
        : mImpl(std::make_unique<Impl>(desc))
    {
    }

    PhysicsWorld::~PhysicsWorld() = default;

    BodyHandle PhysicsWorld::CreateStaticBox(WorldPosition position, const glm::vec3 &halfExtent,
        const glm::quat &rotation)
    {
        if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f || halfExtent.z <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticBox: half extent must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::BoxShape(ToJolt(halfExtent)), position, rotation, false);
    }

    BodyHandle PhysicsWorld::CreateStaticSphere(WorldPosition position, float radius,
        const glm::quat &rotation)
    {
        if (radius <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticSphere: radius must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::SphereShape(radius), position, rotation, false);
    }

    BodyHandle PhysicsWorld::CreateDynamicBox(WorldPosition position, const glm::vec3 &halfExtent,
        const glm::quat &rotation)
    {
        if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f || halfExtent.z <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateDynamicBox: half extent must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::BoxShape(ToJolt(halfExtent)), position, rotation, true);
    }

    BodyHandle PhysicsWorld::CreateDynamicSphere(WorldPosition position, float radius,
        const glm::quat &rotation)
    {
        if (radius <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateDynamicSphere: radius must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::SphereShape(radius), position, rotation, true);
    }

    void PhysicsWorld::DestroyBody(BodyHandle body)
    {
        if (!body.IsValid())
            return;

        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();

        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id))
            return;

        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }

    std::optional<WorldPosition> PhysicsWorld::BodyPosition(BodyHandle body) const
    {
        if (!body.IsValid())
            return std::nullopt;

        // Jolt answers a stale id with the origin, which here is a real place.
        const JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id))
            return std::nullopt;

        return PositionFromJolt(bodies.GetPosition(id));
    }

    glm::quat PhysicsWorld::BodyRotation(BodyHandle body) const
    {
        if (!body.IsValid())
            return kIdentityRotation;

        return FromJolt(mImpl->mSystem.GetBodyInterface().GetRotation(ToBodyId(body)));
    }

    glm::vec3 PhysicsWorld::BodyLinearVelocity(BodyHandle body) const
    {
        if (!body.IsValid())
            return glm::vec3(0.0f);

        return FromJolt(mImpl->mSystem.GetBodyInterface().GetLinearVelocity(ToBodyId(body)));
    }

    glm::vec3 PhysicsWorld::BodyAngularVelocity(BodyHandle body) const
    {
        if (!body.IsValid())
            return glm::vec3(0.0f);

        return FromJolt(mImpl->mSystem.GetBodyInterface().GetAngularVelocity(ToBodyId(body)));
    }

    void PhysicsWorld::SetBodyTransform(BodyHandle body, WorldPosition position, const glm::quat &rotation)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().SetPositionAndRotation(ToBodyId(body),
            ToJolt(position),
            ToJolt(rotation),
            JPH::EActivation::Activate);
    }

    void PhysicsWorld::SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().SetLinearVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().SetAngularVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddLinearVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddLinearAndAngularVelocity(ToBodyId(body),
            JPH::Vec3::sZero(),
            ToJolt(velocity));
    }

    void PhysicsWorld::AddForce(BodyHandle body, const glm::vec3 &force)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddForce(ToBodyId(body), ToJolt(force));
    }

    void PhysicsWorld::AddTorque(BodyHandle body, const glm::vec3 &torque)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddTorque(ToBodyId(body), ToJolt(torque));
    }

    void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3 &impulse)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddImpulse(ToBodyId(body), ToJolt(impulse));
    }

    void PhysicsWorld::AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddAngularImpulse(ToBodyId(body), ToJolt(angularImpulse));
    }

    void PhysicsWorld::AddForceAtOffset(BodyHandle body, const glm::vec3 &force,
        const glm::vec3 &localOffset)
    {
        if (!body.IsValid())
            return;

        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);

        bodies.AddForce(id, ToJolt(force), OffsetToWorldPoint(bodies, id, localOffset));
    }

    void PhysicsWorld::AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse,
        const glm::vec3 &localOffset)
    {
        if (!body.IsValid())
            return;

        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);

        bodies.AddImpulse(id, ToJolt(impulse), OffsetToWorldPoint(bodies, id, localOffset));
    }

    std::size_t PhysicsWorld::BodyCount() const
    {
        return mImpl->mSystem.GetNumBodies();
    }

    void PhysicsWorld::Step(float dt)
    {
        if (dt <= 0.0f)
            return;

        if (mImpl->mBroadPhaseDirty)
        {
            mImpl->mSystem.OptimizeBroadPhase();
            mImpl->mBroadPhaseDirty = false;
        }

        const JPH::EPhysicsUpdateError error = mImpl->mSystem.Update(dt,
            mImpl->mCollisionSteps,
            &mImpl->mTempAllocator,
            &mImpl->mJobSystem);

        if (error != JPH::EPhysicsUpdateError::None)
        {
            MIR_LOG_WARN("PhysicsWorld::Step: Jolt update error bits {}",
                static_cast<uint32_t>(error));
        }
    }
}
