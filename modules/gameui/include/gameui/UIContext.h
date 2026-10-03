/**
 * @file UIContext.h
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, the draw list that is submitted
 *        when the frame ends, and the textures it draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIDrawList.h"
#include "UIId.h"
#include "UILayout.h"
#include "UINavInput.h"
#include "UIState.h"
#include "UITextures.h"
#include "UITypes.h"

#include <input/InputMap.h>
#include <input/InputState.h>

#include <string_view>

namespace mir
{
    //what a button prompt shows for an action on the device the player last used
    struct UIPrompt
    {
        //the binding's short name, e.g. "A" or "Enter"; empty when the action
        //has no binding on that device
        std::string_view mLabel;

        //a gamepad face button, not a keyboard or mouse label
        bool mGamepad = false;
    };

    class UIContext
    {
    public:
        UIContext() = default;

        UIContext(const UIContext &) = delete;
        UIContext &operator=(const UIContext &) = delete;
        UIContext(UIContext &&) = delete;
        UIContext &operator=(UIContext &&) = delete;

        void BeginFrame(const UIRect &area, InputState &input, const InputMap &map, const UIPointer &pointer);

        void EndFrame();

        UIInteraction Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options = {});

        UIDrawList &DrawList() { return mDrawList; }

        UIState &State() { return mState; }
        const UIState &State() const { return mState; }


        const UIRect &Area() const { return mArea; }


        UIRect Bounds() const { return UIRect{0.0f, 0.0f, mArea.width, mArea.height}; }

        UIRect SafeArea() const { return Inset(Bounds(), mSafeAreaInsets); }
        void SetSafeAreaInsets(const UIEdges &insets) { mSafeAreaInsets = insets; }
        const UIEdges &SafeAreaInsets() const { return mSafeAreaInsets; }

        InputState *Input() const { return mInput; }

        const UINavInput &Nav() const { return mNav; }


        bool CancelPressed() const { return mNav.mCancel; }


        bool PausePressed() const { return mNav.mPause; }

        void HoldInput();

        bool HoldsInput() const { return mHoldsInput; }

        bool FocusVisible() const { return mState.FocusVisible(); }

        //only between BeginFrame and EndFrame
        UIPrompt Prompt(std::string_view action) const;

        UIStyle &Style() { return mStyle; }
        const UIStyle &Style() const { return mStyle; }

        UITextures &Textures() { return mTextures; }
        const UITextures &Textures() const { return mTextures; }

    private:
        UITextures mTextures;
        UIDrawList mDrawList;
        UIState mState;
        UIRect mArea;
        UIEdges mSafeAreaInsets;
        InputState *mInput = nullptr;
        const InputMap *mInputMap = nullptr;
        UINavInput mNav;
        UIStyle mStyle;
        bool mInFrame = false;
        bool mHoldInput = false;
        bool mHoldsInput = false;
    };
} // namespace mir
