/**
 * @file InspectorPanel.h
 * @author Rahul Nair
 * @brief Shows and edits the Selection's components - the editor's
 *        "Inspector" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/World.h>
#include <editor/panels/AssetImportPanel.h>

namespace mir
{
    class InspectorPanel
    {
    public:
        void Draw(World &world, const AssetImportView &assets);
    };
} // namespace mir
