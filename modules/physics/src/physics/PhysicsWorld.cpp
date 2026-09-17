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

    glm::vec3 FromJolt(JPH::RVec3Arg v)
    {
        return glm::vec3(static_cast<float>(v.GetX()),
            static_cast<float>(v.GetY()),
            static_cast<float>(v.GetZ()));
    }

#ifdef JPH_DOUBLE_PRECISION
    glm::vec3 FromJolt(JPH::Vec3Arg v)
    {
        return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    }
#endif

    JPH::BodyID ToBodyId(mir::BodyHandle handle)
    {
        return JPH::BodyID(handle.mId);
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

        BodyHandle CreateBody(const JPH::Shape *shape, const glm::vec3 &position,
            const glm::quat &rotation, bool dynamic)
        {
            const JPH::RefConst<JPH::Shape> owned(shape);

            const JPH::BodyCreationSettings settings(owned,
                JPH::RVec3(position.x, position.y, position.z),
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

    BodyHandle PhysicsWorld::CreateStaticBox(const glm::vec3 &position, const glm::vec3 &halfExtent,
        const glm::quat &rotation)
    {
        if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f || halfExtent.z <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticBox: half extent must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::BoxShape(ToJolt(halfExtent)), position, rotation, false);
    }

    BodyHandle PhysicsWorld::CreateStaticSphere(const glm::vec3 &position, float radius,
        const glm::quat &rotation)
    {
        if (radius <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticSphere: radius must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::SphereShape(radius), position, rotation, false);
    }

    BodyHandle PhysicsWorld::CreateDynamicBox(const glm::vec3 &position, const glm::vec3 &halfExtent,
        const glm::quat &rotation)
    {
        if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f || halfExtent.z <= 0.0f)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateDynamicBox: half extent must be positive");
            return {};
        }

        return mImpl->CreateBody(new JPH::BoxShape(ToJolt(halfExtent)), position, rotation, true);
    }

    BodyHandle PhysicsWorld::CreateDynamicSphere(const glm::vec3 &position, float radius,
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

    glm::vec3 PhysicsWorld::BodyPosition(BodyHandle body) const
    {
        if (!body.IsValid())
            return glm::vec3(0.0f);

        return FromJolt(mImpl->mSystem.GetBodyInterface().GetPosition(ToBodyId(body)));
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

    void PhysicsWorld::SetBodyTransform(BodyHandle body, const glm::vec3 &position, const glm::quat &rotation)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().SetPositionAndRotation(ToBodyId(body),
            JPH::RVec3(position.x, position.y, position.z),
            ToJolt(rotation),
            JPH::EActivation::Activate);
    }

    void PhysicsWorld::SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().SetLinearVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3 &impulse)
    {
        if (!body.IsValid())
            return;

        mImpl->mSystem.GetBodyInterface().AddImpulse(ToBodyId(body), ToJolt(impulse));
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
