/**
 * @file UITypes.h
 * @author Quy Dong Ta
 * @brief Value types shared by the game UI: a rect, a color and a style.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>

namespace mir
{
    // Pixels, top-left origin, y down
    struct UIRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        constexpr float Right() const { return x + width; }
        constexpr float Bottom() const { return y + height; }
        constexpr bool IsEmpty() const { return width <= 0.0f || height <= 0.0f; }
        constexpr bool operator==(const UIRect &) const = default;
    };

    // The overlap of a and b; zero-sized rather than negative when they miss
    constexpr UIRect Intersect(const UIRect &a, const UIRect &b)
    {
        const float left = std::max(a.x, b.x);
        const float top = std::max(a.y, b.y);
        const float right = std::min(a.Right(), b.Right());
        const float bottom = std::min(a.Bottom(), b.Bottom());
        return UIRect{left, top, std::max(right - left, 0.0f), std::max(bottom - top, 0.0f)};
    }

    // 0..1 per channel, straight (not premultiplied) alpha
    struct UIColor
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;

        constexpr bool operator==(const UIColor &) const = default;
    };

    inline constexpr UIColor kUIWhite{1.0f, 1.0f, 1.0f, 1.0f};

    struct UIStyle
    {
        UIColor mTextColor = kUIWhite;
        UIColor mPanelColor{0.0f, 0.0f, 0.0f, 0.6f};
        float mPanelRounding = 4.0f;

        // 0: the shared font's own size
        float mFontSize = 0.0f;
    };
} // namespace mir
