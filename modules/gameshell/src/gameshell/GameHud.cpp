/**
 * @file GameHud.cpp
 * @author Quy Dong Ta
 * @brief The HUD over a game in play: the play time, and a pause button that
 *        shows the pause binding for the active device and opens the pause
 *        menu when the pointer clicks it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "gameshell/GameHud.h"

#include "ShellPrompts.h"
#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>
#include <input/DefaultInputMap.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace mir
{
    namespace
    {
        constexpr UIId kPause("hud_pause");

        constexpr float kMargin = 16.0f;
        constexpr float kHeight = 36.0f;
        constexpr float kGap = 8.0f;
        constexpr float kClockWidth = 80.0f;
        constexpr float kCapInset = 6.0f;
        constexpr std::string_view kPauseText = "Pause";
    } // namespace

    bool GameHud::Draw(UIContext &ui, float playSeconds, bool interactive)
    {
        const UIStyle &style = ui.Style();
        const UIPrompt prompt = ui.Prompt(kUIPauseAction);
        const float capWidth = prompt.mLabel.empty() ? 0.0f : shell::CapWidth(prompt.mLabel, kHeight - 2.0f * kCapInset);

        const float buttonWidth = style.mPadding + shell::TextWidth(kPauseText) + kGap + capWidth + kCapInset;
        const UIRect strip = Anchor(Inset(ui.SafeArea(), kMargin), kUITopRight,
                                    kClockWidth + kGap + buttonWidth + 2.0f * kGap, kHeight + 2.0f * kGap);
        Panel(ui, strip);

        UIStack row = UIStack::Row(Inset(strip, kGap), kGap);
        const int seconds = static_cast<int>(std::max(playSeconds, 0.0f));
        Label(ui, UIId("hud_clock"), row.Next(kClockWidth), std::format("{:02}:{:02}", seconds / 60, seconds % 60));

        const UIRect button = row.Rest();
        const bool clicked =
            Button(ui, kPause, button, kPauseText, {.mDisabled = !interactive, .mPointerOnly = true}).mClicked;
        if (!prompt.mLabel.empty())
            shell::DrawCap(ui, UIStack::Row(Inset(button, kCapInset)).Last(capWidth), prompt);
        return clicked;
    }
} // namespace mir
