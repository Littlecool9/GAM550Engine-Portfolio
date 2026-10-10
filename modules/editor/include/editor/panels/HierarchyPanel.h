/**
 * @file HierarchyPanel.h
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/World.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    enum class EntityActionKind
    {
        None,
        Create,
        Delete,
        Duplicate,
        Reparent,
    };

    struct EntityAction
    {
        EntityActionKind mKind = EntityActionKind::None;
        const EntityPresetDesc *mPreset = nullptr; // for create
        Entity mTarget = kNullEntity;              // for delete, duplicate

        // for reparent: a null mBefore means last, and under the current parent that is MoveToLastSibling
        std::vector<Entity> mTargets;
        Entity mParent = kNullEntity;
        Entity mBefore = kNullEntity;
    };

    class HierarchyPanel
    {
    public:
        EntityAction Draw(World &world);

    private:
        // a plain click on an already-selected row waits for release, so a drag can carry the selection
        Entity mDeferredClick = kNullEntity;

        // fixed when the drag starts
        std::vector<Entity> mDragged;

        // the hovered target's action, shown in the drag tooltip
        std::string mDropHint;

        // dropped last frame; App moved them after that Draw, so they are revealed in this one
        std::vector<Entity> mReveal;

        // a collapsed row hovered this long during a drag opens; a failed drop closes it again
        Entity mHoverRow = kNullEntity;
        double mHoverStart = 0.0;
        std::vector<uint32_t> mOpenedDuringDrag; // ImGuiID
        bool mDropDelivered = false;
    };

    Entity NeighborInHierarchy(const World &world, Entity entity);
}
