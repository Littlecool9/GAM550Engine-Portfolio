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

#include <cstddef>
#include <vector>

namespace mir
{
    struct ComponentOps;

    class InspectorPanel
    {
    public:
        void Draw(World &world, const AssetImportView &assets);

        // One field's value
        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        // A component's values copied field by field, so pasting runs each field's setter
        struct Clipboard
        {
            const ComponentOps *mOps = nullptr;
            std::vector<FieldBuffer> mValues; // one per mOps->mFields, in order
        };

    private:
        Clipboard mClipboard;
    };
} // namespace mir
