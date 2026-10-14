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
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    // Complemented so Jolt's default user data reads as no entity, not Entity{0, 0}.
    uint64_t EntityToUserData(mir::Entity entity)
    {
        return entity.IsNull() ? 0 : ~mir::PackEntity(entity);
    }

    mir::Entity EntityFromUserData(uint64_t data)
    {
        const mir::Entity entity = mir::UnpackEntity(~data);
        return entity.IsNull() ? mir::kNullEntity : entity;
    }

    JPH::RVec3 OffsetToWorldPoint(const JPH::BodyInterface &bodies, JPH::BodyID id,
        const glm::vec3 &localOffset)
    {
        const JPH::Vec3 worldOffset = bodies.GetRotation(id) * ToJolt(localOffset);
        return bodies.GetCenterOfMassPosition(id) + worldOffset;
    }

    constexpr JPH::CollisionGroup::GroupID kRestrictedGroup = 1;
    constexpr JPH::CollisionGroup::GroupID kDynamicGroup = 2;

    // Read by Jolt's jobs during a step, changed only between steps.
    class ContactRestriction final : public JPH::GroupFilter
    {
    public:
        bool CanCollide(const JPH::CollisionGroup &a, const JPH::CollisionGroup &b) const override
        {
            const JPH::CollisionGroup &restricted = a.GetGroupID() == kRestrictedGroup ? a : b;
            const JPH::CollisionGroup &other = &restricted == &a ? b : a;
            if (restricted.GetGroupID() != kRestrictedGroup || other.GetGroupID() != kDynamicGroup)
                return true;
            const auto found = mAllowed.find(other.GetSubGroupID());
            return found == mAllowed.end() ||
                   std::binary_search(found->second.begin(), found->second.end(), restricted.GetSubGroupID());
        }

        // By body index, sorted.
        std::unordered_map<JPH::uint32, std::vector<JPH::uint32>> mAllowed;
    };
}

namespace mir
{
    struct SharedHeightField::Data
    {
        // First, so Jolt outlives the shape.
        detail::JoltRuntimeRef runtime;
        JPH::RefConst<JPH::Shape> shape;
        std::optional<HeightFieldInfo> info;
    };

    const HeightFieldInfo *SharedHeightField::Info() const
    {
        return mData && mData->info ? &*mData->info : nullptr;
    }

    SharedHeightField BuildHeightField(const HeightFieldDesc &desc)
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
        const bool sharedRange = !std::isnan(desc.mQuantizationMin) || !std::isnan(desc.mQuantizationMax);
        if (sharedRange && !(std::isfinite(desc.mQuantizationMin) && std::isfinite(desc.mQuantizationMax) &&
                             desc.mQuantizationMin <= desc.mQuantizationMax))
        {
            MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: a quantization range needs finite min <= max");
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

        // Holds Jolt registered while the shape is built, with or without a PhysicsWorld.
        auto data = std::make_shared<SharedHeightField::Data>();
        JPH::HeightFieldShapeSettings settings;
        settings.mScale = JPH::Vec3(spacing, 1.0f, spacing);
        settings.mSampleCount = side;
        settings.mBlockSize = desc.mBlockSize;
        settings.mBitsPerSample = desc.mBitsPerSample;
        if (sharedRange)
        {
            settings.mMinHeightValue = desc.mQuantizationMin;
            settings.mMaxHeightValue = desc.mQuantizationMax;
        }
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
                if (sharedRange && height != kHeightFieldHole &&
                    (height < desc.mQuantizationMin || height > desc.mQuantizationMax))
                {
                    MIR_LOG_ERROR("PhysicsWorld::CreateStaticHeightField: sample ({}, {}) is outside the quantization range",
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

        data->shape = result.Get();
        SharedHeightField built;
        if (result.Get()->GetSubType() != JPH::EShapeSubType::HeightField)
        {
            built.mData = std::move(data);
            return built;
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
        data->info = info;
        built.mData = std::move(data);
        return built;
    }
}

namespace
{
    // Folds Jolt's per sub-shape contacts into one touching state per body pair.
    class ContactTracker final : public JPH::ContactListener
    {
    public:
        // Called on Jolt's job threads with bodies locked: read only IDs and the manifold.
        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
            JPH::ContactSettings &) override
        {
            const JPH::SubShapeIDPair key(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
            const std::scoped_lock lock(mMutex);
            const auto [it, created] = mPairs.try_emplace(PairKey(body1.GetID(), body2.GetID()));
            Pair &pair = it->second;
            if (created)
            {
                pair.mBodyA = std::min(body1.GetID(), body2.GetID());
                pair.mBodyB = std::max(body1.GetID(), body2.GetID());
            }
            if (mSubShapes.insert(key).second)
            {
                ++pair.mSubShapes;
                Queue(it->first, pair);
            }
            Record(pair, body1.GetID(), manifold);
        }

        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
            JPH::ContactSettings &) override
        {
            const std::scoped_lock lock(mMutex);
            if (const auto it = mPairs.find(PairKey(body1.GetID(), body2.GetID())); it != mPairs.end())
                Record(it->second, body1.GetID(), manifold);
        }

        void OnContactRemoved(const JPH::SubShapeIDPair &key) override
        {
            const std::scoped_lock lock(mMutex);
            // Already ended by ForgetBody.
            if (mSubShapes.erase(key) == 0)
                return;
            if (const auto it = mPairs.find(PairKey(key.GetBody1ID(), key.GetBody2ID())); it != mPairs.end())
            {
                --it->second.mSubShapes;
                Queue(it->first, it->second);
            }
        }

        void Flush(const JPH::BodyInterface &bodies, std::vector<mir::CollisionEvent> &out)
        {
            const std::scoped_lock lock(mMutex);
            std::vector<uint64_t> queued;
            queued.swap(mQueued);
            // Sorted: callback order depends on thread timing.
            std::sort(queued.begin(), queued.end());
            for (const uint64_t key : queued)
            {
                const auto it = mPairs.find(key);
                if (it == mPairs.end())
                    continue;

                Pair &pair = it->second;
                pair.mQueued = false;
                if (pair.mSubShapes > 0)
                {
                    if (!pair.mTouching)
                        out.push_back(MakeEvent(mir::CollisionPhase::Begin, pair, bodies));
                    pair.mTouching = true;
                    continue;
                }

                // Jolt drops a sleeping body's contacts; the pair stays touching until a body wakes.
                if (pair.mTouching && !bodies.IsActive(pair.mBodyA) && !bodies.IsActive(pair.mBodyB))
                {
                    Queue(key, pair);
                    continue;
                }

                // Added and removed within one Update: still an impact.
                if (!pair.mTouching)
                    out.push_back(MakeEvent(mir::CollisionPhase::Begin, pair, bodies));
                out.push_back(MakeEvent(mir::CollisionPhase::End, pair, bodies));
                mPairs.erase(it);
            }
        }

        // Call before the body leaves Jolt, while its user data is still readable.
        void ForgetBody(JPH::BodyID body, const JPH::BodyInterface &bodies, std::vector<mir::CollisionEvent> &out)
        {
            const std::scoped_lock lock(mMutex);
            std::vector<uint64_t> ended;
            for (const auto &[key, pair] : mPairs)
            {
                if (pair.mBodyA == body || pair.mBodyB == body)
                    ended.push_back(key);
            }
            std::sort(ended.begin(), ended.end());
            for (const uint64_t key : ended)
            {
                const auto it = mPairs.find(key);
                if (it->second.mTouching)
                    out.push_back(MakeEvent(mir::CollisionPhase::End, it->second, bodies));
                mPairs.erase(it);
            }
            std::erase_if(mSubShapes, [body](const JPH::SubShapeIDPair &key)
                { return key.GetBody1ID() == body || key.GetBody2ID() == body; });
        }

    private:
        struct Pair
        {
            // A has the lower ID.
            JPH::BodyID mBodyA;
            JPH::BodyID mBodyB;
            uint32_t mSubShapes = 0;
            // Begin sent, End not yet.
            bool mTouching = false;
            bool mQueued = false;
            JPH::RVec3 mPoint = JPH::RVec3::sZero();
            // A toward B.
            JPH::Vec3 mNormal = JPH::Vec3::sZero();
        };

        static uint64_t PairKey(JPH::BodyID a, JPH::BodyID b)
        {
            if (b < a)
                std::swap(a, b);
            return (uint64_t(a.GetIndexAndSequenceNumber()) << 32) | b.GetIndexAndSequenceNumber();
        }

        void Queue(uint64_t key, Pair &pair)
        {
            if (!pair.mQueued)
                mQueued.push_back(key);
            pair.mQueued = true;
        }

        // Keeps the latest contact for End, which Jolt reports without one.
        static void Record(Pair &pair, JPH::BodyID body1, const JPH::ContactManifold &manifold)
        {
            const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
            if (count == 0)
                return;

            // Midway between the two surfaces.
            JPH::Vec3 sum = JPH::Vec3::sZero();
            for (JPH::uint i = 0; i < count; ++i)
                sum += manifold.mRelativeContactPointsOn1[i] + manifold.mRelativeContactPointsOn2[i];
            pair.mPoint = manifold.mBaseOffset + sum / float(2 * count);
            // Jolt's normal points from body 1 toward body 2.
            pair.mNormal = body1 == pair.mBodyA ? manifold.mWorldSpaceNormal : -manifold.mWorldSpaceNormal;
        }

        static mir::CollisionEvent MakeEvent(mir::CollisionPhase phase, const Pair &pair,
            const JPH::BodyInterface &bodies)
        {
            mir::CollisionEvent event;
            event.mPhase = phase;
            event.mBodyA = mir::BodyHandle{ pair.mBodyA.GetIndexAndSequenceNumber() };
            event.mBodyB = mir::BodyHandle{ pair.mBodyB.GetIndexAndSequenceNumber() };
            event.mEntityA = EntityFromUserData(bodies.GetUserData(pair.mBodyA));
            event.mEntityB = EntityFromUserData(bodies.GetUserData(pair.mBodyB));
            event.mPoint = PositionFromJolt(pair.mPoint).value_or(mir::WorldPosition{});
            event.mNormal = FromJolt(pair.mNormal);
            return event;
        }

        std::mutex mMutex;
        std::unordered_set<JPH::SubShapeIDPair> mSubShapes;
        std::unordered_map<uint64_t, Pair> mPairs;
        // Pairs whose sub-shape count changed, or that sleep in contact.
        std::vector<uint64_t> mQueued;
    };
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
            mSystem.SetContactListener(&mContacts);
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
            else
                mSystem.GetBodyInterface().SetCollisionGroup(id,
                    JPH::CollisionGroup(nullptr, kDynamicGroup, id.GetIndex()));

            return BodyHandle{ id.GetIndexAndSequenceNumber() };
        }

        // Mass properties are read while the body is still dynamic; Jolt only answers them then.
        struct Held
        {
            JPH::Vec3 linear = JPH::Vec3::sZero();
            JPH::Vec3 angular = JPH::Vec3::sZero();
            float inverseMass = 0.0f;
            JPH::Vec3 inverseInertia = JPH::Vec3::sZero();
            JPH::Quat inertiaRotation = JPH::Quat::sIdentity();
        };

        Held *FindHeld(BodyHandle body)
        {
            const auto found = mHeld.find(body.mId);
            return found == mHeld.end() ? nullptr : &found->second;
        }

        const Held *FindHeld(BodyHandle body) const
        {
            const auto found = mHeld.find(body.mId);
            return found == mHeld.end() ? nullptr : &found->second;
        }

        // A held body is kinematic, so an impulse lands in the velocities it keeps.
        void HeldImpulse(BodyHandle body, Held &held, JPH::Vec3Arg impulse, JPH::Vec3Arg angularImpulse)
        {
            held.linear += impulse * held.inverseMass;
            const JPH::Mat44 axes =
                JPH::Mat44::sRotation(mSystem.GetBodyInterface().GetRotation(ToBodyId(body)) * held.inertiaRotation);
            held.angular += axes.Multiply3x3(held.inverseInertia * axes.Multiply3x3Transposed(angularImpulse));
        }

        // the first in declaration order to outlive other systems
        detail::JoltRuntimeRef mRuntime;

        JPH::TempAllocatorImpl mTempAllocator{ 10 * 1024 * 1024 };
        JPH::JobSystemThreadPool mJobSystem;

        BroadPhaseLayerInterfaceImpl mBroadPhaseLayers;
        ObjectVsBroadPhaseLayerFilterImpl mObjectVsBroadPhase;
        ObjectLayerPairFilterImpl mObjectLayerPairs;

        // Before mSystem, which keeps a pointer to it.
        ContactTracker mContacts;
        std::vector<CollisionEvent> mCollisionEvents;

        JPH::PhysicsSystem mSystem;

        int mCollisionSteps = 1;
        bool mBroadPhaseDirty = false;
        std::size_t mHeightFieldsBuilt = 0;
        std::unordered_map<uint32_t, HeightFieldInfo> mHeightFieldInfo;
        // Shapes of height field bodies, which share them.
        std::unordered_map<uint32_t, SharedHeightField> mHeightFieldShapes;
        JPH::Ref<ContactRestriction> mRestriction = new ContactRestriction();
        std::unordered_map<uint32_t, Held> mHeld;
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
        const SharedHeightField shape = BuildHeightField(desc);
        if (!shape.IsValid())
            return {};
        ++mImpl->mHeightFieldsBuilt;
        return CreateStaticHeightField(shape, desc.mOrigin);
    }

    BodyHandle PhysicsWorld::CreateStaticHeightField(const SharedHeightField &shape, WorldPosition origin)
    {
        if (!shape.IsValid())
            return {};
        // Enhanced internal edge removal keeps planar crossings smooth.
        const BodyHandle body = mImpl->CreateBody(shape.mData->shape, origin, kIdentityRotation, false, true);
        if (!body.IsValid())
            return body;
        if (const HeightFieldInfo *info = shape.Info())
            mImpl->mHeightFieldInfo.emplace(body.mId, *info);
        mImpl->mHeightFieldShapes.emplace(body.mId, shape);
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

        mImpl->mContacts.ForgetBody(id, bodies, mImpl->mCollisionEvents);

        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
        mImpl->mHeightFieldInfo.erase(body.mId);
        mImpl->mHeightFieldShapes.erase(body.mId);
        mImpl->mHeld.erase(body.mId);
        mImpl->mRestriction->mAllowed.erase(id.GetIndex());

        bodies.ActivateBodiesInAABox(bounds, {}, {});
    }

    void PhysicsWorld::RestrictBody(BodyHandle staticBody)
    {
        if (!staticBody.IsValid())
            return;
        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(staticBody);
        if (!bodies.IsAdded(id) || bodies.GetMotionType(id) != JPH::EMotionType::Static)
            return;
        bodies.SetCollisionGroup(id, JPH::CollisionGroup(mImpl->mRestriction, kRestrictedGroup, id.GetIndex()));
    }

    void PhysicsWorld::RestrictContacts(BodyHandle dynamicBody, std::span<const BodyHandle> allowed)
    {
        if (!dynamicBody.IsValid() || !mImpl->mSystem.GetBodyInterface().IsAdded(ToBodyId(dynamicBody)))
            return;
        std::vector<JPH::uint32> &list = mImpl->mRestriction->mAllowed[ToBodyId(dynamicBody).GetIndex()];
        list.clear();
        for (const BodyHandle body : allowed)
            if (body.IsValid())
                list.push_back(ToBodyId(body).GetIndex());
        std::sort(list.begin(), list.end());
    }

    void PhysicsWorld::ClearContactRestriction(BodyHandle dynamicBody)
    {
        if (dynamicBody.IsValid())
            mImpl->mRestriction->mAllowed.erase(ToBodyId(dynamicBody).GetIndex());
    }

    void PhysicsWorld::HoldBody(BodyHandle body)
    {
        if (!body.IsValid() || mImpl->FindHeld(body))
            return;
        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id) || bodies.GetMotionType(id) != JPH::EMotionType::Dynamic)
            return;
        Impl::Held held;
        bodies.GetLinearAndAngularVelocity(id, held.linear, held.angular);
        {
            const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), id);
            if (!lock.Succeeded())
                return;
            const JPH::MotionProperties &motion = *lock.GetBody().GetMotionProperties();
            held.inverseMass = motion.GetInverseMass();
            held.inverseInertia = motion.GetInverseInertiaDiagonal();
            held.inertiaRotation = motion.GetInertiaRotation();
        }
        mImpl->mHeld.emplace(body.mId, held);
        bodies.SetMotionType(id, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
        bodies.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }

    void PhysicsWorld::ResumeBody(BodyHandle body)
    {
        const Impl::Held *held = mImpl->FindHeld(body);
        if (!held)
            return;
        const Impl::Held kept = *held;
        mImpl->mHeld.erase(body.mId);
        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);
        bodies.SetMotionType(id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
        bodies.SetLinearAndAngularVelocity(id, kept.linear, kept.angular);
    }

    bool PhysicsWorld::IsBodyHeld(BodyHandle body) const
    {
        return mImpl->FindHeld(body) != nullptr;
    }

    glm::vec3 PhysicsWorld::Gravity() const
    {
        return FromJolt(mImpl->mSystem.GetGravity());
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
        if (const Impl::Held *held = mImpl->FindHeld(body))
            return FromJolt(held->linear);

        return FromJolt(mImpl->mSystem.GetBodyInterface().GetLinearVelocity(ToBodyId(body)));
    }

    glm::vec3 PhysicsWorld::BodyAngularVelocity(BodyHandle body) const
    {
        if (!body.IsValid())
            return glm::vec3(0.0f);
        if (const Impl::Held *held = mImpl->FindHeld(body))
            return FromJolt(held->angular);

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
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->linear = ToJolt(velocity);
            return;
        }

        mImpl->mSystem.GetBodyInterface().SetLinearVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->angular = ToJolt(velocity);
            return;
        }

        mImpl->mSystem.GetBodyInterface().SetAngularVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->linear += ToJolt(velocity);
            return;
        }

        mImpl->mSystem.GetBodyInterface().AddLinearVelocity(ToBodyId(body), ToJolt(velocity));
    }

    void PhysicsWorld::AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        if (!body.IsValid())
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->angular += ToJolt(velocity);
            return;
        }

        mImpl->mSystem.GetBodyInterface().AddLinearAndAngularVelocity(ToBodyId(body),
            JPH::Vec3::sZero(),
            ToJolt(velocity));
    }

    void PhysicsWorld::AddForce(BodyHandle body, const glm::vec3 &force)
    {
        if (!body.IsValid() || mImpl->FindHeld(body))
            return;

        mImpl->mSystem.GetBodyInterface().AddForce(ToBodyId(body), ToJolt(force));
    }

    void PhysicsWorld::AddTorque(BodyHandle body, const glm::vec3 &torque)
    {
        if (!body.IsValid() || mImpl->FindHeld(body))
            return;

        mImpl->mSystem.GetBodyInterface().AddTorque(ToBodyId(body), ToJolt(torque));
    }

    void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3 &impulse)
    {
        if (!body.IsValid())
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(body, *held, ToJolt(impulse), JPH::Vec3::sZero());
            return;
        }

        mImpl->mSystem.GetBodyInterface().AddImpulse(ToBodyId(body), ToJolt(impulse));
    }

    void PhysicsWorld::AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse)
    {
        if (!body.IsValid())
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(body, *held, JPH::Vec3::sZero(), ToJolt(angularImpulse));
            return;
        }

        mImpl->mSystem.GetBodyInterface().AddAngularImpulse(ToBodyId(body), ToJolt(angularImpulse));
    }

    void PhysicsWorld::AddForceAtOffset(BodyHandle body, const glm::vec3 &force,
        const glm::vec3 &localOffset)
    {
        if (!body.IsValid() || mImpl->FindHeld(body))
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
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            const JPH::Vec3 arm = bodies.GetRotation(id) * ToJolt(localOffset);
            mImpl->HeldImpulse(body, *held, ToJolt(impulse), arm.Cross(ToJolt(impulse)));
            return;
        }

        bodies.AddImpulse(id, ToJolt(impulse), OffsetToWorldPoint(bodies, id, localOffset));
    }

    void PhysicsWorld::SetBodyEntity(BodyHandle body, Entity entity)
    {
        if (!body.IsValid())
            return;

        JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id))
            return;

        bodies.SetUserData(id, EntityToUserData(entity));
    }

    Entity PhysicsWorld::BodyEntity(BodyHandle body) const
    {
        if (!body.IsValid())
            return kNullEntity;

        const JPH::BodyInterface &bodies = mImpl->mSystem.GetBodyInterface();
        const JPH::BodyID id = ToBodyId(body);
        if (!bodies.IsAdded(id))
            return kNullEntity;

        return EntityFromUserData(bodies.GetUserData(id));
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

        mImpl->mContacts.Flush(mImpl->mSystem.GetBodyInterface(), mImpl->mCollisionEvents);
    }

    std::vector<CollisionEvent> PhysicsWorld::DrainCollisionEvents()
    {
        return std::exchange(mImpl->mCollisionEvents, {});
    }
}
