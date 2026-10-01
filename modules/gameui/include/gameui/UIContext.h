/**
 * @file UIContext.h
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, and the draw list that is submitted
 *        when the frame ends.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIDrawList.h"
#include "UIId.h"
#include "UINavInput.h"
#include "UIState.h"
#include "UITypes.h"

#include <input/InputState.h>

namespace mir
{
    class UIContext
    {
    public:
        UIContext() = default;

        UIContext(const UIContext &) = delete;
        UIContext &operator=(const UIContext &) = delete;
        UIContext(UIContext &&) = delete;
        UIContext &operator=(UIContext &&) = delete;

        void BeginFrame(const UIRect &area, InputState &input, const UIPointer &pointer);

        void EndFrame();

        UIInteraction Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options = {});

        UIDrawList &DrawList() { return mDrawList; }

        UIState &State() { return mState; }
        const UIState &State() const { return mState; }

        //the area passed to BeginFrame
        const UIRect &Area() const { return mArea; }

        //only between BeginFrame and EndFrame; null outside a frame
        InputState *Input() const { return mInput; }

        //the UI actions read from the input at BeginFrame while zero outside a frame
        const UINavInput &Nav() const { return mNav; }

        //cancel for specific frame, belongs to whatevr is drawing UI
        bool CancelPressed() const { return mNav.mCancel; }

        UIStyle &Style() { return mStyle; }
        const UIStyle &Style() const { return mStyle; }

    private:
        UIDrawList mDrawList;
        UIState mState;
        UIRect mArea;
        InputState *mInput = nullptr;
        UINavInput mNav;
        UIStyle mStyle;
        bool mInFrame = false;
    };
} // namespace mir
