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

namespace mir
{
    enum class EntityActionKind
    {
        None,
        Create,
    };

    struct EntityAction
    {
        EntityActionKind mKind = EntityActionKind::None;
        const EntityPresetDesc *mPreset = nullptr; // Create: null is an empty entity
    };

    class HierarchyPanel
    {
    public:
        EntityAction Draw(World &world);
    };
}
