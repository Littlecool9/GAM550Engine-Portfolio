/**
 * @file HierarchyIndex.cpp
 * @author Sumin Park
 * @brief The scene graph: parent and child edges, owned as a World resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/HierarchyIndex.h"

#include "core/log/Assert.h"

#include <algorithm>

namespace mir
{
    HierarchyIndex::Node *HierarchyIndex::Find(Entity entity)
    {
        if (entity.IsNull() || entity.mIndex >= mNodes.size())
            return nullptr;

        Node &node = mNodes[entity.mIndex];
        return node.owner == entity ? &node : nullptr;
    }

    const HierarchyIndex::Node *HierarchyIndex::Find(Entity entity) const
    {
        if (entity.IsNull() || entity.mIndex >= mNodes.size())
            return nullptr;

        const Node &node = mNodes[entity.mIndex];
        return node.owner == entity ? &node : nullptr;
    }

    HierarchyIndex::Node &HierarchyIndex::GetNode(Entity entity)
    {
        if (entity.mIndex >= mNodes.size())
            mNodes.resize(entity.mIndex + 1);

        Node &node = mNodes[entity.mIndex];

        // override destroyed entity
        if (!(node.owner == entity))
        {
            node.owner = entity;
            node.parent = kNullEntity;
            node.children.clear();
        }

        return node;
    }

    Entity HierarchyIndex::ParentOf(Entity entity) const
    {
        const Node *node = Find(entity);
        return node ? node->parent : kNullEntity;
    }

    std::span<const Entity> HierarchyIndex::ChildrenOf(Entity entity) const
    {
        const Node *node = Find(entity);
        return node ? std::span<const Entity>(node->children) : std::span<const Entity>{};
    }

    std::span<const Entity> HierarchyIndex::Roots() const
    {
        return mRoots;
    }

    uint32_t HierarchyIndex::DepthOf(Entity entity) const
    {
        uint32_t depth = 0;
        for (Entity cursor = ParentOf(entity); !cursor.IsNull(); cursor = ParentOf(cursor))
        {
            if (++depth >= kMaxHierarchyDepth)
                break;
        }

        return depth;
    }

    uint32_t HierarchyIndex::HeightFrom(Entity entity, uint32_t guard) const
    {
        const Node *node = Find(entity);
        if (node == nullptr || guard >= kMaxHierarchyDepth)
            return 0;

        uint32_t best = 0;
        for (const Entity child : node->children)
            best = std::max(best, 1 + HeightFrom(child, guard + 1));

        return best;
    }

    uint32_t HierarchyIndex::HeightOf(Entity entity) const
    {
        return HeightFrom(entity, 0);
    }

    bool HierarchyIndex::IsAncestorOf(Entity ancestor, Entity entity) const
    {
        if (ancestor.IsNull() || entity.IsNull())
            return false;

        uint32_t depth = 0;
        for (Entity cursor = entity; !cursor.IsNull(); cursor = ParentOf(cursor))
        {
            if (cursor == ancestor)
                return true;

            if (++depth >= kMaxHierarchyDepth)
                break;
        }

        return false;
    }

    void HierarchyIndex::Unlink(Entity child, Entity parent)
    {
        Node *node = Find(parent);
        if (node == nullptr)
            return;

        // Order-preserving erase
        const auto found = std::find(node->children.begin(), node->children.end(), child);
        if (found != node->children.end())
            node->children.erase(found);
    }

    void HierarchyIndex::EraseRoot(Entity entity)
    {
        const auto found = std::find(mRoots.begin(), mRoots.end(), entity);
        if (found != mRoots.end())
            mRoots.erase(found);
    }

    void HierarchyIndex::AddRoot(Entity entity)
    {
        GetNode(entity);
        mRoots.push_back(entity);
    }

    bool HierarchyIndex::SetParent(Entity child, Entity parent)
    {
        if (child.IsNull())
            return false;

        if (!parent.IsNull())
        {
            if (child == parent)
                return false;

            // cycle
            if (IsAncestorOf(child, parent))
                return false;

            // deeper than maxDepth
            if (DepthOf(parent) + 1 + HeightOf(child) >= kMaxHierarchyDepth)
                return false;
        }

        const Entity previous = ParentOf(child);
        if (previous == parent)
            return true;

        if (!previous.IsNull())
            Unlink(child, previous);
        else
            EraseRoot(child);

        GetNode(child).parent = parent;

        if (!parent.IsNull())
            GetNode(parent).children.push_back(child);
        else
            mRoots.push_back(child);

        return true;
    }

    std::vector<Entity> HierarchyIndex::DetachChildren(Entity entity)
    {
        Node *node = Find(entity);
        if (node == nullptr)
            return {};

        for (const Entity child : node->children)
        {
            if (Node *orphan = Find(child))
                orphan->parent = kNullEntity;
        }

        return std::move(node->children);
    }

    void HierarchyIndex::Remove(Entity entity)
    {
        Node *node = Find(entity);
        if (node == nullptr)
            return;

        const Entity parent = node->parent;

        if (!parent.IsNull())
            Unlink(entity, parent);
        else
            EraseRoot(entity);

        for (const Entity child : node->children)
        {
            if (Node *orphan = Find(child))
                orphan->parent = kNullEntity;
        }

        node->owner = kNullEntity;
        node->parent = kNullEntity;

        node->children.clear();
    }

    std::size_t HierarchyIndex::NodeCount() const
    {
        std::size_t count = 0;
        for (const Node &node : mNodes)
        {
            if (!node.owner.IsNull())
                ++count;
        }

        return count;
    }
}
