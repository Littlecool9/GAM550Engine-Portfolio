/**
 * @file TransformHierarchy.h
 * @author Sumin Park
 * @brief Resolving Transform through the scene graph into WorldTransform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/ecs/Entity.h"
#include "core/ecs/HierarchyIndex.h"
#include "core/ecs/System.h"
#include "core/ecs/World.h"
#include "core/ecs/components/Transform.h"
#include "core/ecs/components/WorldTransform.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    // The entity's pose in the fixed Scene world; a dead entity is the identity at the origin.
    WorldPose ResolveWorld(World &world, Entity entity);

    // Adds Transform and its WorldTransform cache
    Transform &AddTransform(World &world,
                            Entity entity,
                            const Transform &transform = Transform{},
                            Entity parent = kNullEntity);

    // child local transform is left untouched
    bool SetParent(World &world, Entity child, Entity parent);

    // Parent of `entity`, or null. Reads the graph without creating it.
    Entity ParentOf(const World &world, Entity entity);

    // True if `ancestor` is `entity` or any transitive parent of it.
    bool IsAncestorOf(const World &world, Entity ancestor, Entity entity);

    // Installs the HierarchyIndex resource and the destroy hook
    HierarchyIndex &InstallHierarchy(World &world);

    // Calls `fn(Entity)` for each direct child of `parent`, in insertion order.
    // fn may reparent or destry anything
    template <typename Fn>
    void ForEachChild(World &world, Entity parent, Fn &&fn)
    {
        HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        if (index == nullptr)
            return;

        // iterate on snapshot
        const std::span<const Entity> children = index->ChildrenOf(parent);
        std::vector<Entity> snapshot(children.begin(), children.end());

        for (const Entity child : snapshot)
        {
            if (world.IsAlive(child) && index->ParentOf(child) == parent)
                fn(child);
        }
    }

    // Calls `fn(Entity)` for each child of `parent`, dfs, in insertion order.
    // fn may reparent or destry anything
    template <typename Fn>
    void ForEachDescendant(World &world, Entity root, Fn &&fn)
    {
        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        if (index == nullptr)
            return;

        std::vector<Entity> pending;
        const std::span<const Entity> roots = index->ChildrenOf(root);
        pending.insert(pending.end(), roots.begin(), roots.end());

        for (std::size_t i = 0; i < pending.size(); ++i)
        {
            const std::span<const Entity> children = index->ChildrenOf(pending[i]);
            pending.insert(pending.end(), children.begin(), children.end());
        }

        for (const Entity entity : pending)
            fn(entity);
    }

    // Updates every WorldTransform once per frame
    class TransformPropagateSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<Transform, WorldTransform> *mQuery = nullptr;
    };
}
