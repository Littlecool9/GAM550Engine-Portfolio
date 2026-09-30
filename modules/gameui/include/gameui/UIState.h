/**
 * @file UIState.h
 * @author Quy Dong Ta
 * @brief Which widget is under the pointer (hot), which one it pressed
 *        (active, latched until release), which one has focus, the hit test,
 *        and the explicit links focus moves along.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIId.h"
#include "UINavInput.h"
#include "UITypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
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



    using UINavLinks = std::array<UIId, 4>;

    struct UIWidgetOptions
    {
        UINavLinks mLinks{};

        bool mHidden = false;

        bool mDisabled = false;
    };

    struct UIInteraction
    {

        bool mHovered = false;

        bool mHeld = false;

        //pointer release over the widget, or UIConfirm while it has focus
        bool mClicked = false;

        //as of the last EndFrame
        bool mFocused = false;
    };

    class UIState
    {
    public:

        void BeginFrame(const UIRect &area, const UIPointer &pointer, const UINavInput &nav = {});


        void EndFrame();

        UIInteraction Interact(UIId id, const UIRect &rect, const UIRect &clip, const UIWidgetOptions &options = {});

        bool HitTest(const UIRect &rect, const UIRect &clip) const;

        UIId FollowLink(UIId from, UINavDirection direction) const;

        UIId Hot() const { return mHot; }
        UIId Active() const { return mActive; }


        UIId Focus() const { return mFocus; }

        void SetFocus(UIId id) { mFocus = id; }

        float PointerX() const { return mPointerX; }
        float PointerY() const { return mPointerY; }

        size_t Collisions() const { return mCollisions; }

    private:
        struct Declared
        {
            UINavLinks mLinks{};
            bool mEnabled = true;
        };

        bool IsEnabled(UIId id) const;

        UIRect mArea;
        float mPointerX = 0.0f;
        float mPointerY = 0.0f;
        bool mDown = false;

        bool mPressed = false;

        //window pixels, not local, so a dock drag under a still pointer is not a move
        float mLastPointerX = 0.0f;
        float mLastPointerY = 0.0f;
        bool mHasLastPointer = false;
        bool mPointerMoved = false;

        UINavStepper mNavStepper;
        std::optional<UINavDirection> mNavDirection;
        bool mConfirm = false;

        UIId mHot;
        UIId mActive;
        UIId mFocus;

        UIId mNextHot;

        bool mActiveSeen = false;

        // this frame's widgets by id; the first declaration of a colliding id wins
        std::unordered_map<uint64_t, Declared> mDeclared;
        UIId mFirstEnabled;
        size_t mCollisions = 0;

        std::unordered_set<uint64_t> mLoggedCollisions;
    };
} // namespace mir
