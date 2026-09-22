/**
 * @file Selection.h
 * @author Rahul Nair
 * @brief World resource holding the editor's current selection - written
 *        by the Hierarchy panel and viewport picking, read by the
 *        Inspector panel and the transform gizmo.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <algorithm>
#include <vector>

namespace mir
{
    struct Selection
    {
        std::vector<Entity> mSelected;

        bool Contains(Entity e) const
        {
            return std::find(mSelected.begin(), mSelected.end(), e) != mSelected.end();
        }

        Entity Primary() const { return mSelected.empty() ? kNullEntity : mSelected.back(); }
    };

    inline void ApplyClick(Selection &selection, Entity clicked, bool additive)
    {
        if (clicked.IsNull())
        {
            if (!additive)
                selection.mSelected.clear();
            return;
        }

        if (!additive)
        {
            selection.mSelected = { clicked };
            return;
        }

        if (const auto it = std::find(selection.mSelected.begin(), selection.mSelected.end(), clicked);
            it != selection.mSelected.end())
            selection.mSelected.erase(it);
        else
            selection.mSelected.push_back(clicked);
    }
}
