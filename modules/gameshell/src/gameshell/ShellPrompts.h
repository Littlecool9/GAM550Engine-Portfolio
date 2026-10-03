/**
 * @file ShellPrompts.h
 * @author Quy Dong Ta
 * @brief Button prompts for the shell's screens: a key cap or pad pill with
 *        the binding's label, then what it does.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIContext.h>
#include <gameui/UILayout.h>

#include <algorithm>
#include <string_view>

namespace mir::shell
{

    inline float TextWidth(std::string_view text) { return 8.0f * static_cast<float>(text.size()); }


    inline float CapWidth(std::string_view label, float height) { return std::max(height, TextWidth(label) + 12.0f); }


    inline void DrawCap(UIContext &ui, const UIRect &rect, const UIPrompt &prompt)
    {
        ui.DrawList().Rect(rect, UIColor{0.9f, 0.9f, 0.9f, 0.9f}, prompt.mGamepad ? rect.height * 0.5f : 3.0f);
        ui.DrawList().Text(rect.x + 6.0f, rect.y + (rect.height - ui.Style().mTextHeight) * 0.5f, prompt.mLabel,
                           UIColor{0.0f, 0.0f, 0.0f, 1.0f}, ui.Style().mFontSize);
    }


    inline void DrawPrompt(UIContext &ui, UIStack &strip, std::string_view action, std::string_view text)
    {
        const UIPrompt prompt = ui.Prompt(action);
        if (prompt.mLabel.empty())
            return;

        const UIRect bounds = strip.Bounds();
        const float capWidth = CapWidth(prompt.mLabel, bounds.height);
        UIStack parts = UIStack::Row(strip.Last(capWidth + 6.0f + TextWidth(text)), 6.0f);
        DrawCap(ui, parts.Next(capWidth), prompt);

        const UIRect caption = parts.Rest();
        ui.DrawList().Text(caption.x, caption.y + (caption.height - ui.Style().mTextHeight) * 0.5f, text,
                           ui.Style().mTextColor, ui.Style().mFontSize);
    }
} // namespace mir::shell
