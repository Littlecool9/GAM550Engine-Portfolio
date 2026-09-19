/**
 * @file HierarchyPanel.h
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/World.h>

namespace mir
{
    class HierarchyPanel
    {
    public:
        void Draw(World &world);
    };
}
