/**
 * @file TransformHierarchy.cpp
 * @author Sumin Park
 * @brief Resolving Transform through the scene graph into WorldTransform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/TransformHierarchy.h"

#include "core/ecs/CommandBuffer.h"
#include "core/ecs/Query.h"
#include "core/ecs/World.h"
#include "core/log/Assert.h"
#include "core/log/Log.h"

#include <optional>

namespace mir
{
    namespace detail
    {
        // Only thing allowed to write WorldTransform's private members
        struct TransformResolver
        {
            static void Store(WorldTransform &target,
                const WorldPose &pose,
                uint32_t localVersion,
                uint32_t parentVersion)
            {
                target.mPose = pose;
                target.mLocalVersion = localVersion;
                target.mParentVersion = parentVersion;
                target.mDirty = false;

                if (++target.mVersion == 0)
                    target.mVersion = 1;
            }

            static bool IsClean(const WorldTransform &target, uint32_t localVersion, uint32_t parentVersion)
            {
                return !target.mDirty &&
                    target.mLocalVersion == localVersion &&
                    target.mParentVersion == parentVersion;
            }

            static void Invalidate(WorldTransform &target) { target.mDirty = true; }
        };

        // Only thing allowed to call HierarchyIndex's private mutators
        struct HierarchyMutator
        {
            static bool SetParent(HierarchyIndex &index, Entity child, Entity parent, Entity before)
            {
                return index.SetParent(child, parent, before);
            }

            static bool MoveToLastSibling(HierarchyIndex &index, Entity entity)
            {
                return index.MoveToLastSibling(entity);
            }

            static void Remove(HierarchyIndex &index, Entity entity) { index.Remove(entity); }

            static std::vector<Entity> DetachChildren(HierarchyIndex &index, Entity entity)
            {
                return index.DetachChildren(entity);
            }

            static void AddRoot(HierarchyIndex &index, Entity entity) { index.AddRoot(entity); }
        };
    }

    namespace
    {
        Entity LiveParentOf(World &world, const HierarchyIndex *index, Entity entity)
        {
            if (index == nullptr)
                return kNullEntity;

            const Entity parent = index->ParentOf(entity);
            return world.IsAlive(parent) ? parent : kNullEntity;
        }

        // force recalculation
        void InvalidateCache(World &world, Entity entity)
        {
            if (WorldTransform *cache = world.GetComponent<WorldTransform>(entity))
                detail::TransformResolver::Invalidate(*cache);
        }

        // A root composes under the identity pose at the origin, which is exact
        // in either precision, so one path serves roots and children.
        WorldPose Compose(const WorldPose &parent, const Transform &local, Entity entity)
        {
            if (const std::optional<WorldPose> pose = TryComposePose(parent, local.LinearMatrix(), local.Position()))
                return *pose;

            MIR_LOG_WARN("ResolveWorld: entity {} leaves the finite world; kept at its parent's position",
                entity.mIndex);
            return WorldPose{ parent.position, parent.linear * local.LinearMatrix() };
        }

        constexpr float kMinParentAxis = 1e-12f;

        // The local values that hold `child`'s world pose under `parent`. A zero-scale
        // parent counts as translation only. nullopt when the position does not fit.
        std::optional<Transform> TryKeepWorld(World &world, Entity child, Entity parent)
        {
            const WorldPose childPose = ResolveWorld(world, child);
            WorldPose parentPose = parent.IsNull() ? WorldPose{} : ResolveWorld(world, parent);

            for (int axis = 0; axis < 3; ++axis)
            {
                if (glm::length(parentPose.linear[axis]) < kMinParentAxis)
                    parentPose.linear = glm::mat3(1.0f);
            }

            const std::optional<ParentLocalTranslation> position = TryWorldToParentLocal(parentPose, childPose.position);
            if (!position)
                return std::nullopt;

            glm::quat rotation{};
            glm::vec3 scale{};
            DecomposeRotationScale(glm::inverse(parentPose.linear) * childPose.linear, rotation, scale);
            return Transform(position->Meters(), rotation, scale);
        }

        void OnEntityCreated(World &world, Entity entity, void *)
        {
            if (HierarchyIndex *index = world.TryResource<HierarchyIndex>())
                detail::HierarchyMutator::AddRoot(*index, entity);
        }

        // destroy all destroyed entity's children
        void OnEntityDestroyed(World &world, Entity entity, void *)
        {
            HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr)
                return;

            const std::vector<Entity> children = detail::HierarchyMutator::DetachChildren(*index, entity);

            for (const Entity child : children)
            {
                MIR_ASSERT(world.IsAlive(child), "OnEntityDestroyed: dead entity left in the scene graph");

                if (world.IsAlive(child))
                    world.DestroyEntity(child);
            }

            detail::HierarchyMutator::Remove(*index, entity);
        }
    }

    HierarchyIndex &InstallHierarchy(World &world)
    {
        if (!world.HasResource<HierarchyIndex>())
        {
            HierarchyIndex &index = world.EmplaceResource<HierarchyIndex>();

            // entities made before install are roots, in slot order
            world.ForEachAliveEntity([&index](Entity entity) { detail::HierarchyMutator::AddRoot(index, entity); });
        }

        world.AddCreateHook(&OnEntityCreated);
        world.AddDestroyHook(&OnEntityDestroyed);

        return world.Resource<HierarchyIndex>();
    }

    Entity ParentOf(const World &world, Entity entity)
    {
        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        return index ? index->ParentOf(entity) : kNullEntity;
    }

    bool IsAncestorOf(const World &world, Entity ancestor, Entity entity)
    {
        if (ancestor.IsNull() || entity.IsNull())
            return false;

        if (ancestor == entity)
            return true;

        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        return index != nullptr && index->IsAncestorOf(ancestor, entity);
    }

    WorldPose ResolveWorld(World &world, Entity entity)
    {
        // if dead, identity
        if (!world.IsAlive(entity))
            return WorldPose{};

        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();

        Entity chain[kMaxHierarchyDepth];
        uint32_t depth = 0;

        for (Entity cursor = entity; !cursor.IsNull(); cursor = LiveParentOf(world, index, cursor))
        {
            MIR_ASSERT(depth < kMaxHierarchyDepth,
                "ResolveWorld: hierarchy deeper than {} - the graph should have refused this",
                kMaxHierarchyDepth);

            if (depth >= kMaxHierarchyDepth)
                break;

            chain[depth++] = cursor;
        }

        WorldPose accumulated;

        uint32_t parentVersion = 0;

        // False once an ancestor has no WorldTransform to stamp.
        bool cacheable = true;

        // Now back down, root first.
        for (uint32_t i = depth; i-- > 0;)
        {
            const Entity current = chain[i];
            const Transform *local = world.GetComponent<Transform>(current);
            WorldTransform *cache = world.GetComponent<WorldTransform>(current);

            const uint32_t localVersion = local ? local->Version() : 0;

            if (cache == nullptr)
            {
                accumulated = local ? Compose(accumulated, *local, current) : accumulated;
                cacheable = false;
                continue;
            }

            if (cacheable && detail::TransformResolver::IsClean(*cache, localVersion, parentVersion))
            {
                accumulated = cache->Pose();
                parentVersion = cache->Version();
                continue;
            }

            accumulated = local ? Compose(accumulated, *local, current) : accumulated;
            detail::TransformResolver::Store(*cache, accumulated, localVersion, parentVersion);
            parentVersion = cache->Version();
            cacheable = true;
        }

        return accumulated;
    }

    bool SetParent(World &world, Entity child, Entity parent, Entity before, bool keepWorld)
    {
        if (!world.IsAlive(child))
        {
            MIR_LOG_WARN("SetParent: child is not alive");
            return false;
        }

        if (!parent.IsNull() && !world.IsAlive(parent))
        {
            MIR_LOG_WARN("SetParent: parent is not alive");
            return false;
        }

        HierarchyIndex &index = InstallHierarchy(world);

        // a reorder under the same parent keeps local values, so it never round-trips them
        std::optional<Transform> kept;
        if (keepWorld && index.ParentOf(child) != parent && world.Has<Transform>(child))
        {
            kept = TryKeepWorld(world, child, parent);
            if (!kept)
            {
                MIR_LOG_WARN("SetParent: refused - the child's world position does not fit under the new parent");
                return false;
            }
        }

        if (!detail::HierarchyMutator::SetParent(index, child, parent, before))
        {
            MIR_LOG_WARN("SetParent: refused - self-parenting, a cycle, deeper than "
                "kMaxHierarchyDepth ({}), or `before` is not a sibling under the parent",
                kMaxHierarchyDepth);
            return false;
        }

        if (kept)
        {
            // setters, not assignment, so the version bumps
            Transform &local = *world.GetComponent<Transform>(child);
            local.SetPosition(kept->Position());
            local.SetRotation(kept->Rotation());
            local.SetScale(kept->Scale());
        }

        InvalidateCache(world, child);
        return true;
    }

    bool MoveToLastSibling(World &world, Entity entity)
    {
        if (!world.IsAlive(entity))
        {
            MIR_LOG_WARN("MoveToLastSibling: entity is not alive");
            return false;
        }

        return detail::HierarchyMutator::MoveToLastSibling(InstallHierarchy(world), entity);
    }

    Transform &AddTransform(World &world, Entity entity, const Transform &transform, Entity parent)
    {
        InstallHierarchy(world);

        if (world.Has<Transform>(entity))
            *world.GetComponent<Transform>(entity) = transform;
        else
            world.AddComponent<Transform>(entity, transform);

        if (!world.Has<WorldTransform>(entity))
            world.AddComponent<WorldTransform>(entity, WorldTransform{});

        if (!parent.IsNull())
            SetParent(world, entity, parent, kNullEntity, false);

        return *world.GetComponent<Transform>(entity);
    }

    void TransformPropagateSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, WorldTransform>();
    }

    void TransformPropagateSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;
        mQuery->ForEach([&world](Entity entity, Transform &, WorldTransform &)
            {
                ResolveWorld(world, entity);
            });
    }
}
