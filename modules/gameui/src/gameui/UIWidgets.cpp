/**
 * @file UIWidgets.cpp
 * @author Quy Dong Ta
 * @brief The core widgets, Label, Button, Image, Bar, Toggle and Slider: each
 *        declares itself through UIContext::Interact, draws its parts as the
 *        style's sprites or, without them, grey-box rects, draws its text,
 *        and returns what happened to it this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIWidgets.h>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        UIWidgetResult Declare(UIContext &ui, UIId id, const UIRect &rect, const UIWidgetOptions &options)
        {
            return UIWidgetResult{ui.Interact(id, rect, options)};
        }

        UIWidgetResult DeclareDisplay(UIContext &ui, UIId id, const UIRect &rect, const UIDisplayOptions &options)
        {
            UIWidgetOptions widget = options.mWidget;
            if (!options.mFocusable)
                widget.mDisabled = true;
            return Declare(ui, id, rect, widget);
        }

        bool IsEnabled(const UIWidgetOptions &options) { return !options.mHidden && !options.mDisabled; }


        struct Part
        {
            UIColor mColor;
            const UISprite *mSprite = nullptr;
        };


        void DrawPart(UIContext &ui, const UIRect &rect, const Part &part, float rounding)
        {
            if (part.mSprite != nullptr && part.mSprite->IsValid())
                ui.DrawList().Sprite(rect, *part.mSprite);
            else
                ui.DrawList().Rect(rect, part.mColor, rounding);
        }

        Part Track(const UIStyle &style) { return Part{style.mTrackColor, &style.mTrackSprite}; }
        Part Fill(const UIStyle &style) { return Part{style.mFillColor, &style.mFillSprite}; }
        Part Face(const UIStyle &style) { return Part{style.mWidgetColor, &style.mWidgetSprite}; }

  
        void FocusRing(UIContext &ui, const UIRect &rect, const UIInteraction &interaction)
        {
            if (!interaction.mShowFocus)
                return;

            const UIStyle &style = ui.Style();
            const float t = style.mFocusThickness;
            DrawPart(ui, UIRect{rect.x - t, rect.y - t, rect.width + 2.0f * t, rect.height + 2.0f * t},
                     Part{style.mFocusColor, &style.mFocusSprite}, style.mWidgetRounding + t);
        }


        Part FacePart(const UIStyle &style, const UIInteraction &interaction, bool enabled)
        {
            const auto state = [&](UIColor color, const UISprite &sprite) {
                return Part{color, sprite.IsValid() ? &sprite : &style.mWidgetSprite};
            };
            if (enabled && interaction.mHeld && interaction.mHovered)
                return state(style.mWidgetHeldColor, style.mWidgetHeldSprite);
            if (enabled && interaction.mHovered)
                return state(style.mWidgetHoveredColor, style.mWidgetHoveredSprite);
            return Face(style);
        }

        UIColor TextColor(const UIStyle &style, bool enabled) { return enabled ? style.mTextColor : style.mDisabledTextColor; }


        void DrawText(UIContext &ui, const UIRect &rect, float x, std::string_view text, UIColor color)
        {
            const UIStyle &style = ui.Style();
            const float height = style.mFontSize > 0.0f ? style.mFontSize : style.mTextHeight;
            ui.DrawList().Text(x, rect.y + (rect.height - height) * 0.5f, text, color, style.mFontSize);
        }

        float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }
    } // namespace

    void Panel(UIContext &ui, const UIRect &rect)
    {
        const UIStyle &style = ui.Style();
        DrawPart(ui, rect, Part{style.mPanelColor, &style.mPanelSprite}, style.mPanelRounding);
    }

    UIWidgetResult Label(UIContext &ui, UIId id, const UIRect &rect, std::string_view text,
                         const UIDisplayOptions &options)
    {
        const UIWidgetResult result = DeclareDisplay(ui, id, rect, options);
        if (options.mWidget.mHidden)
            return result;

        FocusRing(ui, rect, result);
        const bool dimmed = options.mFocusable && options.mWidget.mDisabled;
        DrawText(ui, rect, rect.x + ui.Style().mPadding, text, TextColor(ui.Style(), !dimmed));
        return result;
    }

    UIWidgetResult Button(UIContext &ui, UIId id, const UIRect &rect, std::string_view text,
                          const UIWidgetOptions &options)
    {
        const UIWidgetResult result = Declare(ui, id, rect, options);
        if (options.mHidden)
            return result;

        const UIStyle &style = ui.Style();
        const bool enabled = IsEnabled(options);
        FocusRing(ui, rect, result);
        DrawPart(ui, rect, FacePart(style, result, enabled), style.mWidgetRounding);
        DrawText(ui, rect, rect.x + style.mPadding, text, TextColor(style, enabled));
        return result;
    }

    UIWidgetResult Image(UIContext &ui, UIId id, const UIRect &rect, const UISprite &sprite, UIColor tint,
                         const UIDisplayOptions &options)
    {
        const UIWidgetResult result = DeclareDisplay(ui, id, rect, options);
        if (options.mWidget.mHidden)
            return result;

        FocusRing(ui, rect, result);
        if (sprite.IsValid())
            ui.DrawList().Sprite(rect, sprite, tint);
        else
            ui.DrawList().Rect(rect, tint);
        return result;
    }

    UIWidgetResult Bar(UIContext &ui, UIId id, const UIRect &rect, float fraction, const UIDisplayOptions &options)
    {
        const UIWidgetResult result = DeclareDisplay(ui, id, rect, options);
        if (options.mWidget.mHidden)
            return result;

        const UIStyle &style = ui.Style();
        FocusRing(ui, rect, result);
        DrawPart(ui, rect, Track(style), style.mWidgetRounding);

        const float filled = rect.width * Clamp01(fraction);
        if (filled > 0.0f)
            DrawPart(ui, UIRect{rect.x, rect.y, filled, rect.height}, Fill(style), style.mWidgetRounding);
        return result;
    }

    UIWidgetResult Toggle(UIContext &ui, UIId id, const UIRect &rect, std::string_view text, bool &value,
                          const UIWidgetOptions &options)
    {
        UIWidgetResult result = Declare(ui, id, rect, options);
        if (result.mClicked)
        {
            value = !value;
            result.mChanged = true;
        }
        if (options.mHidden)
            return result;

        const UIStyle &style = ui.Style();
        const bool enabled = IsEnabled(options);
        FocusRing(ui, rect, result);
        DrawPart(ui, rect, FacePart(style, result, enabled), style.mWidgetRounding);

        const float pad = style.mPadding;
        const float side = std::max(rect.height - 2.0f * pad, 0.0f);
        const UIRect box{rect.x + pad, rect.y + pad, side, side};
        DrawPart(ui, box, value ? Fill(style) : Track(style), style.mWidgetRounding);

        DrawText(ui, rect, box.Right() + pad, text, TextColor(style, enabled));
        return result;
    }

    UIWidgetResult Slider(UIContext &ui, UIId id, const UIRect &rect, float &value, float min, float max, float step,
                          const UIWidgetOptions &options)
    {
        UIWidgetOptions adjusting = options;
        adjusting.mAdjustsX = true;
        UIWidgetResult result = Declare(ui, id, rect, adjusting);

        const bool enabled = IsEnabled(options);
        const float range = max - min;
        if (enabled && range > 0.0f && (result.mHeld || result.mAdjust != 0))
        {
            float next = value;
            if (result.mHeld && rect.width > 0.0f)
                next = min + range * Clamp01((ui.State().PointerX() - rect.x) / rect.width);
            if (result.mAdjust != 0)
                next = value + static_cast<float>(result.mAdjust) * (step > 0.0f ? step : range / 20.0f);

            if (step > 0.0f)
                next = min + std::round((next - min) / step) * step;
            next = std::clamp(next, min, max);

            if (next != value)
            {
                value = next;
                result.mChanged = true;
            }
        }
        if (options.mHidden)
            return result;

        const UIStyle &style = ui.Style();
        FocusRing(ui, rect, result);
        DrawPart(ui, rect, Track(style), style.mWidgetRounding);

        const float fraction = range > 0.0f ? Clamp01((value - min) / range) : 0.0f;
        const float filled = rect.width * fraction;
        if (filled > 0.0f)
            DrawPart(ui, UIRect{rect.x, rect.y, filled, rect.height}, enabled ? Fill(style) : Face(style),
                     style.mWidgetRounding);

        const float knobWidth = std::min(2.0f * style.mPadding, rect.width);
        const float knobX = rect.x + (rect.width - knobWidth) * fraction;
        DrawPart(ui, UIRect{knobX, rect.y, knobWidth, rect.height}, FacePart(style, result, enabled),
                 style.mWidgetRounding);
        return result;
    }
} // namespace mir
