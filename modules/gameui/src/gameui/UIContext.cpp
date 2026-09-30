/**
 * @file UIContext.cpp
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, and the draw list that is submitted
 *        when the frame ends.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>

#include "UIBackend.h"
#include <core/log/Assert.h>

namespace mir
{
    void UIContext::BeginFrame(const UIRect &area, InputState &input, const UIPointer &pointer)
    {
        MIR_ASSERT(!mInFrame, "UIContext::BeginFrame twice without EndFrame");

        mArea = area;
        mInput = &input;
        mNav = ReadNavInput(input);
        mDrawList.Reset(area.width, area.height);
        mState.BeginFrame(area, pointer, mNav);
        mInFrame = true;
    }

    void UIContext::EndFrame()
    {
        MIR_ASSERT(mInFrame, "UIContext::EndFrame without BeginFrame");
        MIR_ASSERT(mDrawList.ClipDepth() == 0, "UIContext::EndFrame with {} PushClip unpopped",
                   mDrawList.ClipDepth());

        if (mInFrame && !mArea.IsEmpty())
            detail::SubmitToBackground(mArea, mDrawList);

        if (mInFrame)
            mState.EndFrame();

        mInput = nullptr;
        mNav = UINavInput{};
        mInFrame = false;
    }

    UIInteraction UIContext::Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options)
    {
        MIR_ASSERT(mInFrame, "UIContext::Interact outside BeginFrame/EndFrame");

        return mState.Interact(id, rect, mDrawList.CurrentClip(), options);
    }
} // namespace mir
