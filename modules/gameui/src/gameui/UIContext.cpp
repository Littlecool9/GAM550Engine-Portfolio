/**
 * @file UIContext.cpp
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, the draw list that is submitted
 *        when the frame ends, and the textures it draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>

#include "UIBackend.h"
#include <core/log/Assert.h>
#include <input/PromptLabel.h>

namespace mir
{
    void UIContext::BeginFrame(const UIRect &area, InputState &input, const InputMap &map, const UIPointer &pointer)
    {
        MIR_ASSERT(!mInFrame, "UIContext::BeginFrame twice without EndFrame");

        mArea = area;
        mInput = &input;
        mInputMap = &map;
        mNav = ReadNavInput(input);
        mDrawList.Reset(area.width, area.height);

        const bool focusVisible = input.ActiveDevice() != DeviceKind::Mouse;
        mState.BeginFrame(area, pointer, mNav, focusVisible);
        mInFrame = true;
    }

    void UIContext::EndFrame()
    {
        MIR_ASSERT(mInFrame, "UIContext::EndFrame without BeginFrame");
        MIR_ASSERT(mDrawList.ClipDepth() == 0, "UIContext::EndFrame with {} PushClip unpopped",
                   mDrawList.ClipDepth());

        if (mInFrame && !mArea.IsEmpty())
            detail::SubmitToBackground(mArea, mDrawList, mTextures);

        if (mInFrame)
        {
            mState.EndFrame();


            mHoldsInput = mHoldInput;
            mInput->SetGameInputHeld(mHoldsInput);
        }
        mHoldInput = false;

        mInput = nullptr;
        mInputMap = nullptr;
        mNav = UINavInput{};
        mInFrame = false;
    }

    void UIContext::HoldInput()
    {
        MIR_ASSERT(mInFrame, "UIContext::HoldInput outside BeginFrame/EndFrame");
        mHoldInput = mInFrame;
    }

    UIInteraction UIContext::Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options)
    {
        MIR_ASSERT(mInFrame, "UIContext::Interact outside BeginFrame/EndFrame");

        return mState.Interact(id, rect, mDrawList.CurrentClip(), options);
    }

    UIPrompt UIContext::Prompt(std::string_view action) const
    {
        MIR_ASSERT(mInFrame, "UIContext::Prompt outside BeginFrame/EndFrame");
        if (!mInFrame)
            return {};

        const DeviceKind device = mInput->ActiveDevice();
        return UIPrompt{PromptLabel(*mInputMap, action, device), device == DeviceKind::Gamepad};
    }
} // namespace mir
