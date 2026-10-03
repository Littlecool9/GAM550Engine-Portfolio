/**
 * @file UILayout.h
 * @author Quy Dong Ta
 * @brief Layout helpers over UIRect: inset, anchor to an edge or corner, and
 *        rows and columns cut from a rect one after another. Each takes a rect
 *        and returns rects, so they nest in any order and keep nothing between
 *        frames.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace mir
{

    constexpr UIRect Inset(const UIRect &rect, const UIEdges &insets)
    {
        const float left = rect.x + insets.mLeft;
        const float top = rect.y + insets.mTop;
        const float right = std::max(rect.Right() - insets.mRight, left);
        const float bottom = std::max(rect.Bottom() - insets.mBottom, top);
        return UIRect{left, top, right - left, bottom - top};
    }

    constexpr UIRect Inset(const UIRect &rect, float all) { return Inset(rect, UIEdges{all, all, all, all}); }

    struct UIAnchor
    {
        float x = 0.0f;
        float y = 0.0f;

        constexpr bool operator==(const UIAnchor &) const = default;
    };

    inline constexpr UIAnchor kUITopLeft{0.0f, 0.0f};
    inline constexpr UIAnchor kUITop{0.5f, 0.0f};
    inline constexpr UIAnchor kUITopRight{1.0f, 0.0f};
    inline constexpr UIAnchor kUILeft{0.0f, 0.5f};
    inline constexpr UIAnchor kUICenter{0.5f, 0.5f};
    inline constexpr UIAnchor kUIRight{1.0f, 0.5f};
    inline constexpr UIAnchor kUIBottomLeft{0.0f, 1.0f};
    inline constexpr UIAnchor kUIBottom{0.5f, 1.0f};
    inline constexpr UIAnchor kUIBottomRight{1.0f, 1.0f};

    constexpr UIRect Anchor(const UIRect &container, UIAnchor anchor, float width, float height)
    {
        return UIRect{container.x + (container.width - width) * anchor.x,
                      container.y + (container.height - height) * anchor.y, width, height};
    }

    enum class UIAxis : uint8_t
    {
        Horizontal,
        Vertical,
    };


    class UIStack
    {
    public:
        constexpr UIStack(const UIRect &bounds, UIAxis axis, float gap = 0.0f)
            : mBounds(bounds), mAxis(axis), mGap(gap), mStart(0.0f), mEnd(AxisLength(bounds, axis))
        {
        }

        static constexpr UIStack Row(const UIRect &bounds, float gap = 0.0f)
        {
            return UIStack(bounds, UIAxis::Horizontal, gap);
        }

        static constexpr UIStack Column(const UIRect &bounds, float gap = 0.0f)
        {
            return UIStack(bounds, UIAxis::Vertical, gap);
        }


        static constexpr float Length(std::initializer_list<float> sizes, float gap = 0.0f)
        {
            float length = 0.0f;
            for (const float size : sizes)
                length += size;
            return sizes.size() > 1 ? length + gap * static_cast<float>(sizes.size() - 1) : length;
        }

        constexpr UIRect Next(float size)
        {
            const float begin = Begin();
            const float taken = std::clamp(size, 0.0f, std::max(End() - begin, 0.0f));
            mStart = begin + taken;
            mHasStart = true;
            return Cut(begin, taken);
        }


        constexpr UIRect Last(float size)
        {
            const float end = End();
            const float taken = std::clamp(size, 0.0f, std::max(end - Begin(), 0.0f));
            mEnd = end - taken;
            mHasEnd = true;
            return Cut(mEnd, taken);
        }

        constexpr UIRect Rest() { return Next(Remaining()); }

        constexpr float Remaining() const { return std::max(End() - Begin(), 0.0f); }

        constexpr const UIRect &Bounds() const { return mBounds; }

    private:
        static constexpr float AxisLength(const UIRect &rect, UIAxis axis)
        {
            return axis == UIAxis::Horizontal ? rect.width : rect.height;
        }


        constexpr float Begin() const { return mHasStart ? mStart + mGap : mStart; }
        constexpr float End() const { return mHasEnd ? mEnd - mGap : mEnd; }

        constexpr UIRect Cut(float offset, float size) const
        {
            if (mAxis == UIAxis::Horizontal)
                return UIRect{mBounds.x + offset, mBounds.y, size, mBounds.height};
            return UIRect{mBounds.x, mBounds.y + offset, mBounds.width, size};
        }

        UIRect mBounds;
        UIAxis mAxis;
        float mGap;

        float mStart;
        float mEnd;
        bool mHasStart = false;
        bool mHasEnd = false;
    };
} // namespace mir
