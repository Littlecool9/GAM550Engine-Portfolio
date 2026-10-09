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
#include <iterator>
#include <span>
#include <vector>

namespace mir
{
    struct Selection
    {
        std::vector<Entity> mSelected;

        // where a shift-click range starts: the last entity clicked without shift
        Entity mAnchor = kNullEntity;

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

        selection.mAnchor = clicked;
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

    // Selects rows from the anchor through clicked, so clicked ends up primary; keep adds them (Ctrl+Shift)
    inline void ApplyRangeClick(Selection &selection, Entity clicked, std::span<const Entity> rows, bool keep)
    {
        const auto clickedIt = std::find(rows.begin(), rows.end(), clicked);
        if (clickedIt == rows.end())
            return;

        // an anchor that is gone or hidden starts the range at the click
        auto anchorIt = std::find(rows.begin(), rows.end(), selection.mAnchor);
        if (anchorIt == rows.end())
        {
            selection.mAnchor = clicked;
            anchorIt = clickedIt;
        }

        std::vector<Entity> range;
        if (anchorIt <= clickedIt)
            range.assign(anchorIt, clickedIt + 1);
        else
            range.assign(std::make_reverse_iterator(anchorIt + 1), std::make_reverse_iterator(clickedIt));

        if (!keep)
        {
            selection.mSelected = std::move(range);
            return;
        }

        std::erase_if(selection.mSelected,
            [&](Entity e) { return std::find(range.begin(), range.end(), e) != range.end(); });
        selection.mSelected.insert(selection.mSelected.end(), range.begin(), range.end());
    }
}
