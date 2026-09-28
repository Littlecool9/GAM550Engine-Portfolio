/**
 * @file UIContext.cpp
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, and
 *        the draw list that is submitted when the frame ends.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>

#include "UIBackend.h"
#include <core/log/Assert.h>

namespace mir
{
    void UIContext::BeginFrame(const UIRect &area, InputState &input)
    {
        MIR_ASSERT(!mInFrame, "UIContext::BeginFrame twice without EndFrame");

        mArea = area;
        mInput = &input;
        mDrawList.Reset(area.width, area.height);
        mInFrame = true;
    }

    void UIContext::EndFrame()
    {
        MIR_ASSERT(mInFrame, "UIContext::EndFrame without BeginFrame");
        MIR_ASSERT(mDrawList.ClipDepth() == 0, "UIContext::EndFrame with {} PushClip unpopped",
                   mDrawList.ClipDepth());

        if (mInFrame && !mArea.IsEmpty())
            detail::SubmitToBackground(mArea, mDrawList);

        mInput = nullptr;
        mInFrame = false;
    }
} // namespace mir
