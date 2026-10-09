/**
 * @file HierarchyIndex.h
 * @author Sumin Park
 * @brief The scene graph: parent and child edges, owned as a World resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    namespace detail
    {
        struct HierarchyMutator;
    }

    // Chains deeper than this are thought as cycles
    inline constexpr uint32_t kMaxHierarchyDepth = 64;

    class HierarchyIndex
    {
    public:
        // Null when `entity` is a root or absent from the graph.
        Entity ParentOf(Entity entity) const;

        // Direct children in insertion order
        std::span<const Entity> ChildrenOf(Entity entity) const;

        // Entities with no parent, in creation order; a reparent to root appends
        std::span<const Entity> Roots() const;

        // True if `ancestor` is `entity` or any transitive parent of it.
        bool IsAncestorOf(Entity ancestor, Entity entity) const;

        // Levels above `entity`. 0 for a root.
        uint32_t DepthOf(Entity entity) const;

        // Levels below `entity`. 0 for a leaf.
        uint32_t HeightOf(Entity entity) const;

        bool CanSetParent(Entity child, Entity parent, Entity before) const;

        std::size_t NodeCount() const;

    private:
        friend struct detail::HierarchyMutator;

        // Links `child` under `parent` before sibling `before`, or last when `before` is null.
        bool SetParent(Entity child, Entity parent, Entity before);

        // False when `entity` is not in the graph
        bool MoveToLastSibling(Entity entity);

        // Detatches `entity` from hierarchy
        // children are removed through hook
        void Remove(Entity entity);

        std::vector<Entity> DetachChildren(Entity entity);

        // Appends a newly created entity to the roots
        void AddRoot(Entity entity);

        struct Node
        {
            Entity owner;

            Entity parent;
            std::vector<Entity> children;
        };

        uint32_t HeightFrom(Entity entity, uint32_t guard) const;

        Node *Find(Entity entity);
        const Node *Find(Entity entity) const;

        // Inserts if no node
        Node &GetNode(Entity entity);

        void Unlink(Entity child, Entity parent);

        void EraseRoot(Entity entity);

        // The children of `parent`, or the roots when `parent` is null
        std::vector<Entity> &SiblingsUnder(Entity parent);
        const std::vector<Entity> *FindSiblingsUnder(Entity parent) const;

        // Dense, indexed by Entity::mIndex
        std::vector<Node> mNodes;

        std::vector<Entity> mRoots;
    };
}
