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
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <unordered_map>

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

    // A Jolt of another precision would round or widen every world position on
    // the way in. CMake and JoltRuntime check the package; this checks what this
    // file compiled against.
    static_assert(sizeof(JPH::Real) == sizeof(mir::WorldReal),
        "JPH_DOUBLE_PRECISION must match MIR_LARGE_WORLD (the large-world vcpkg feature)");

    JPH::RVec3 ToJolt(mir::WorldPosition position)
    {
        const mir::WorldVec3 meters = position.Meters();
        return JPH::RVec3(meters.x, meters.y, meters.z);
    }

    std::optional<mir::WorldPosition> PositionFromJolt(JPH::RVec3Arg v)
    {
        return mir::WorldPosition::FromMeters(mir::WorldVec3(v.GetX(), v.GetY(), v.GetZ()));
    }

    JPH::BodyID ToBodyId(mir::BodyHandle handle)
    {
        return JPH::BodyID(handle.mId);
    }

    static_assert(mir::kHeightFieldHole == JPH::HeightFieldShapeConstants::cNoCollisionValue);

    // Sub shape ids hold 15 bits each of x and z.
    constexpr uint32_t kMaxHeightFieldSide = 32768;

    // Covers resting contact plus Jolt's 2 cm speculative distance.
    constexpr float kWakeMargin = 0.1f;

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
            const glm::quat &rotation, bool dynamic, bool enhancedInternalEdgeRemoval = false)
        {
            const JPH::RefConst<JPH::Shape> owned(shape);

            JPH::BodyCreationSettings settings(owned,
                ToJolt(position),
                ToJolt(rotation),
                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                dynamic ? Layers::kMoving : Layers::kNonMoving);
            settings.mEnhancedInternalEdgeRemoval = enhancedInternalEdgeRemoval;

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
        std::size_t mHeightFieldsBuilt = 0;
        std::unordered_map<uint32_t, HeightFieldInfo> mHeightFieldInfo;
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

    BodyHandle PhysicsWorld::CreateStaticHeightField(const HeightFieldDesc &desc)
    {
        const uint32_t stride = desc.mSampleStride;
        if (desc.mCountX < 2 || desc.mCountZ < 2 ||
            desc.mSamples.size() != static_cast<std::size_t>(desc.mCountX) * desc.mCountZ)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: needs countX * countZ samples, at least 2 x 2");
            return {};
        }
        if (stride == 0 || (desc.mCountX - 1) % stride != 0 || (desc.mCountZ - 1) % stride != 0)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: count - 1 must be a multiple of the sample stride");
            return {};
        }
        const float spacing = desc.mSpacing * static_cast<float>(stride);
        if (!(desc.mSpacing > 0.0f) || !std::isfinite(spacing))
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: spacing must be positive and finite");
            return {};
        }
        // Jolt 5.5's triangle walk breaks from block size 3 (terrain ADR 0017).
        if ((desc.mBlockSize != 2 && desc.mBlockSize != 4) || desc.mBitsPerSample < 1 || desc.mBitsPerSample > 8)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: block size must be 2 or 4, bits per sample in [1, 8]");
            return {};
        }

        const uint32_t columns = (desc.mCountX - 1) / stride + 1;
        const uint32_t rows = (desc.mCountZ - 1) / stride + 1;
        // Jolt's square is at least two blocks; every added sample is a hole.
        const uint32_t side = std::max({columns, rows, 2 * desc.mBlockSize});
        if (side > kMaxHeightFieldSide - kMaxHeightFieldSide % desc.mBlockSize)
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: {} x {} samples exceed Jolt's {} per side",
                columns, rows, kMaxHeightFieldSide);
            return {};
        }

        JPH::HeightFieldShapeSettings settings;
        settings.mScale = JPH::Vec3(spacing, 1.0f, spacing);
        settings.mSampleCount = side;
        settings.mBlockSize = desc.mBlockSize;
        settings.mBitsPerSample = desc.mBitsPerSample;
        settings.mHeightSamples.resize(static_cast<std::size_t>(side) * side, kHeightFieldHole);
        for (uint32_t z = 0; z < rows; ++z)
        {
            for (uint32_t x = 0; x < columns; ++x)
            {
                const float height = desc.mSamples[static_cast<std::size_t>(z) * stride * desc.mCountX +
                                                  static_cast<std::size_t>(x) * stride];
                if (!std::isfinite(height))
                {
                    MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: sample ({}, {}) is not finite",
                        x * stride, z * stride);
                    return {};
                }
                settings.mHeightSamples[static_cast<std::size_t>(z) * side + x] = height;
            }
        }

        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError())
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: {}", result.GetError().c_str());
            return {};
        }

        if (result.Get()->GetSubType() != JPH::EShapeSubType::HeightField)
        {
            ++mImpl->mHeightFieldsBuilt;
            return mImpl->CreateBody(result.Get(), desc.mOrigin, kIdentityRotation, false, true);
        }
        const auto *field = static_cast<const JPH::HeightFieldShape *>(result.Get().GetPtr());
        HeightFieldInfo info{field->GetSampleCount(), field->GetStats().mSizeBytes};
        for (uint32_t z = 0; z < rows; ++z)
            for (uint32_t x = 0; x < columns; ++x)
            {
                const float input = desc.mSamples[size_t(z) * stride * desc.mCountX + size_t(x) * stride];
                if (input == kHeightFieldHole) continue;
                const JPH::Vec3 decoded = field->GetPosition(x, z);
                if (field->IsNoCollision(x, z) || !std::isfinite(decoded.GetY()))
                {
                    MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: a valid sample decoded to a hole");
                    return {};
                }
                info.mMaxQuantizationError = std::max(info.mMaxQuantizationError, std::abs(double(decoded.GetY()) - input));
                info.mMaxHorizontalError = std::max({info.mMaxHorizontalError,
                    std::abs(double(decoded.GetX()) - double(x) * desc.mSpacing * stride),
                    std::abs(double(decoded.GetZ()) - double(z) * desc.mSpacing * stride)});
                ++info.mValidSamples;
            }
        info.mExtentX = field->GetPosition(columns - 1, 0).GetX();
        info.mExtentZ = field->GetPosition(0, rows - 1).GetZ();
        ++mImpl->mHeightFieldsBuilt;
        // Enhanced internal edge removal keeps planar crossings smooth.
        const BodyHandle body = mImpl->CreateBody(result.Get(), desc.mOrigin, kIdentityRotation, false, true);
        if (body.IsValid()) mImpl->mHeightFieldInfo.emplace(body.mId, info);
        return body;
    }

    void PhysicsWorld::DestroyBody(BodyHandle body)
    {
        if (!body.IsValid())
            return;

        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();

        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id))
            return;

        // Jolt's removal wakes nothing.
        JPH::AABox bounds = bodies.GetTransformedShape(id).GetWorldSpaceBounds();
        bounds.ExpandBy(JPH::Vec3::sReplicate(kWakeMargin));

        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
        mImpl->mHeightFieldInfo.erase(body.mId);

        bodies.ActivateBodiesInAABox(bounds, {}, {});
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

    std::optional<RayHit> PhysicsWorld::RayCast(WorldPosition origin, const glm::vec3 &direction,
        float maxDistance) const
    {
        const float length = glm::length(direction);
        const glm::vec3 span = direction * (maxDistance / length);
        if (!(maxDistance > 0.0f) || !(length > 0.0f) || !std::isfinite(span.x) || !std::isfinite(span.y) ||
            !std::isfinite(span.z))
            return std::nullopt;

        const JPH::RRayCast ray(ToJolt(origin), ToJolt(span));
        JPH::RayCastResult hit;
        if (!mImpl->mSystem.GetNarrowPhaseQuery().CastRay(ray, hit))
            return std::nullopt;

        // origin + fraction * span: the float product is added in world precision
        const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
        const std::optional<WorldPosition> position = PositionFromJolt(point);
        if (!position)
            return std::nullopt;

        RayHit result;
        result.mBody = BodyHandle{ hit.mBodyID.GetIndexAndSequenceNumber() };
        result.mPoint = *position;
        result.mDistance = hit.mFraction * maxDistance;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            result.mNormal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));

        return result;
    }

    std::optional<HeightFieldInfo> PhysicsWorld::HeightFieldInfoOf(BodyHandle body) const
    {
        if (!body.IsValid())
            return std::nullopt;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), ToBodyId(body));
        if (!lock.SucceededAndIsInBroadPhase())
            return std::nullopt;

        const JPH::Shape *shape = lock.GetBody().GetShape();
        if (shape->GetSubType() != JPH::EShapeSubType::HeightField)
            return std::nullopt;

        const auto found = mImpl->mHeightFieldInfo.find(body.mId);
        return found == mImpl->mHeightFieldInfo.end() ? std::nullopt : std::optional(found->second);
    }

    std::size_t PhysicsWorld::HeightFieldsBuilt() const
    {
        return mImpl->mHeightFieldsBuilt;
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
