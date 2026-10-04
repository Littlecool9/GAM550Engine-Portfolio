/**
 * @file UIState.cpp
 * @author Quy Dong Ta
 * @brief Which widget is under the pointer (hot), which one it pressed
 *        (active, latched until release), which one has focus, the hit test,
 *        and the explicit links focus moves along.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIState.h>

#include <core/log/Assert.h>
#include <core/log/Log.h>

namespace mir
{
    void UIState::BeginFrame(const UIRect &area, const UIPointer &pointer, const UINavInput &nav, bool focusVisible)
    {
        mArea = area;

        mPointerX = pointer.mX - area.x;
        mPointerY = pointer.mY - area.y;

        mPointerMoved = mHasLastPointer && (pointer.mX != mLastPointerX || pointer.mY != mLastPointerY);
        mLastPointerX = pointer.mX;
        mLastPointerY = pointer.mY;
        mHasLastPointer = true;

        mConfirm = nav.mConfirm && !pointer.mDown && !mDown;
        mNavDirection = mNavStepper.Step(nav);

        mRevealFocus = focusVisible && !mFocusVisible;
        mFocusVisible = focusVisible;
        if (mRevealFocus)
            mConfirm = false;

        mPressed = pointer.mDown && !mDown;
        mDown = pointer.mDown;

        mNavAdjusted = false;
        mNextHot = UIId{};
        mActiveSeen = false;
        mDeclared.clear();
        mFirstFocusable = UIId{};
        mCollisions = 0;
    }

    void UIState::EndFrame()
    {

        if (!mDown || !mActiveSeen)
            mActive = UIId{};


        if (mPressed)
            mActive = mNextHot;

        mHot = mNextHot;

        if ((mPointerMoved || mPressed) && IsFocusable(mNextHot))
            mFocus = mNextHot;

        if (mNavDirection && !mNavAdjusted)
        {
            if (mDeclared.contains(mFocus.Value()))
            {

                if (!mRevealFocus)
                    mFocus = FollowLink(mFocus, *mNavDirection);
            }
            else if (!mFirstFocusable.IsNull())
                mFocus = mFirstFocusable;
        }
    }

    UIInteraction UIState::Interact(UIId id, const UIRect &rect, const UIRect &clip, const UIWidgetOptions &options)
    {
        MIR_ASSERT(!id.IsNull(), "UIState::Interact with the null UIId");
        if (id.IsNull())
            return {};

        const bool enabled = !options.mHidden && !options.mDisabled;
        const bool focusable = enabled && !options.mPointerOnly;
        if (!mDeclared.try_emplace(id.Value(), Declared{options.mLinks, focusable}).second)
        {
            ++mCollisions;
            if (mLoggedCollisions.insert(id.Value()).second)
                MIR_LOG_WARN("gameui: UIId {:#018x} was given to more than one widget in a frame, so they act as one; "
                             "give each a different name or index",
                             id.Value());
        }

        if (focusable && mFirstFocusable.IsNull())
            mFirstFocusable = id;

        const bool over = !options.mHidden && HitTest(rect, clip);


        const bool reachable = mActive.IsNull() ? !mDown || mPressed : mActive == id;
        if (over && reachable)
            mNextHot = id;

        UIInteraction result;
        result.mHovered = over && mHot == id;

        result.mFocused = mFocus == id && !options.mPointerOnly;
        result.mShowFocus = result.mFocused && mFocusVisible;

        if (mActive == id)
        {
            mActiveSeen = true;
            result.mHeld = mDown;

            result.mClicked = !mDown && over && enabled;
        }

        if (mConfirm && result.mFocused && enabled)
            result.mClicked = true;

        if (options.mAdjustsX && result.mFocused && enabled && mNavDirection && !mRevealFocus)
        {
            if (*mNavDirection == UINavDirection::Left)
                result.mAdjust = -1;
            else if (*mNavDirection == UINavDirection::Right)
                result.mAdjust = 1;
            mNavAdjusted = mNavAdjusted || result.mAdjust != 0;
        }

        return result;
    }

    UIId UIState::FollowLink(UIId from, UINavDirection direction) const
    {
        const size_t index = static_cast<size_t>(direction);

        const auto start = mDeclared.find(from.Value());
        if (start == mDeclared.end())
            return from;

        UIId next = start->second.mLinks[index];
        for (size_t steps = 0; !next.IsNull() && steps < mDeclared.size(); ++steps)
        {
            const auto it = mDeclared.find(next.Value());
            if (it == mDeclared.end())
                return from;
            if (it->second.mFocusable)
                return next;
            next = it->second.mLinks[index];
        }
        return from;
    }

    bool UIState::IsFocusable(UIId id) const
    {
        const auto it = mDeclared.find(id.Value());
        return it != mDeclared.end() && it->second.mFocusable;
    }

    bool UIState::HitTest(const UIRect &rect, const UIRect &clip) const
    {
        const UIRect root{0.0f, 0.0f, mArea.width, mArea.height};
        return Contains(Intersect(Intersect(rect, clip), root), mPointerX, mPointerY);
    }
} // namespace mir
