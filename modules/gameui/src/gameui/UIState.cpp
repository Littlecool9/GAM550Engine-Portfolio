/**
 * @file UIState.cpp
 * @author Quy Dong Ta
 * @brief Which widget is under the pointer (hot) and which one it pressed
 *        (active, latched until release), and the hit test behind both.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIState.h>

#include <core/log/Assert.h>
#include <core/log/Log.h>

namespace mir
{
    void UIState::BeginFrame(const UIRect &area, const UIPointer &pointer)
    {
        mArea = area;

        mPointerX = pointer.mX - area.x;
        mPointerY = pointer.mY - area.y;

        mPressed = pointer.mDown && !mDown;
        mDown = pointer.mDown;

        mNextHot = UIId{};
        mActiveSeen = false;
        mDeclared.clear();
        mCollisions = 0;
    }

    void UIState::EndFrame()
    {

        if (!mDown || !mActiveSeen)
            mActive = UIId{};


        if (mPressed)
            mActive = mNextHot;

        mHot = mNextHot;
    }

    UIInteraction UIState::Interact(UIId id, const UIRect &rect, const UIRect &clip)
    {
        MIR_ASSERT(!id.IsNull(), "UIState::Interact with the null UIId");
        if (id.IsNull())
            return {};

        if (!mDeclared.insert(id.Value()).second)
        {
            ++mCollisions;
            if (mLoggedCollisions.insert(id.Value()).second)
                MIR_LOG_WARN("gameui: UIId {:#018x} was given to more than one widget in a frame, so they act as one; "
                             "give each a different name or index",
                             id.Value());
        }

        const bool over = HitTest(rect, clip);


        const bool reachable = mActive.IsNull() ? !mDown || mPressed : mActive == id;
        if (over && reachable)
            mNextHot = id;

        UIInteraction result;
        result.mHovered = over && mHot == id;

        if (mActive == id)
        {
            mActiveSeen = true;
            result.mHeld = mDown;

            result.mClicked = !mDown && over;
        }

        return result;
    }

    bool UIState::HitTest(const UIRect &rect, const UIRect &clip) const
    {
        const UIRect root{0.0f, 0.0f, mArea.width, mArea.height};
        return Contains(Intersect(Intersect(rect, clip), root), mPointerX, mPointerY);
    }
} // namespace mir
