/**
 * @file UIState.h
 * @author Quy Dong Ta
 * @brief Which widget is under the pointer (hot) and which one it pressed
 *        (active, latched until release), and the hit test behind both.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIId.h"
#include "UITypes.h"

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace mir
{

    struct UIPointer
    {
        float mX = 0.0f;
        float mY = 0.0f;

        // The primary button
        bool mDown = false;
    };


    struct UIInteraction
    {

        bool mHovered = false;

        bool mHeld = false;

        bool mClicked = false;
    };

    class UIState
    {
    public:

        void BeginFrame(const UIRect &area, const UIPointer &pointer);


        void EndFrame();

        UIInteraction Interact(UIId id, const UIRect &rect, const UIRect &clip);

        bool HitTest(const UIRect &rect, const UIRect &clip) const;

        UIId Hot() const { return mHot; }
        UIId Active() const { return mActive; }

        float PointerX() const { return mPointerX; }
        float PointerY() const { return mPointerY; }

        size_t Collisions() const { return mCollisions; }

    private:
        UIRect mArea;
        float mPointerX = 0.0f;
        float mPointerY = 0.0f;
        bool mDown = false;

        bool mPressed = false;

        UIId mHot;
        UIId mActive;

        UIId mNextHot;

        bool mActiveSeen = false;

        std::unordered_set<uint64_t> mDeclared;
        size_t mCollisions = 0;

        std::unordered_set<uint64_t> mLoggedCollisions;
    };
} // namespace mir
