/**
 * @file PauseMenu.cpp
 * @author Quy Dong Ta
 * @brief The menu a game pauses into: Resume, Restart and Quit over a dimmed
 *        screen, driven by gamepad, mouse or keyboard, built on gameui's
 *        public API alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "gameshell/PauseMenu.h"

#include "ShellPrompts.h"
#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>
#include <input/DefaultInputMap.h>

#include <algorithm>

namespace mir
{
    namespace
    {
        constexpr UIId kResume("pause_resume");
        constexpr UIId kRestart("pause_restart");
        constexpr UIId kQuit("pause_quit");

        constexpr UIColor kDim{0.0f, 0.0f, 0.0f, 0.55f};

        constexpr float kWidth = 360.0f;
        constexpr float kPadding = 16.0f;
        constexpr float kGap = 12.0f;
        constexpr float kRow = 40.0f;
        constexpr float kPromptRow = 24.0f;
        constexpr float kPromptGap = 16.0f;

        // title, the three buttons, the prompts
        constexpr float kHeight = UIStack::Length({kRow, kRow, kRow, kRow, kPromptRow}, kGap) + 2.0f * kPadding;
    } // namespace

    PauseMenuAction PauseMenu::Draw(UIContext &ui, bool canOpen)
    {
        if (!canOpen)
        {
            mOpen = false;
            mOpenRequested = false;
            return PauseMenuAction::None;
        }

        PauseMenuAction action = PauseMenuAction::None;
        if (!mOpen)
        {
            if (!ui.PausePressed() && !mOpenRequested)
                return PauseMenuAction::None;

            mOpen = true;
            mOpenRequested = false;
            ui.State().SetFocus(kResume);
            action = PauseMenuAction::Opened;
        }

        else if (ui.PausePressed() || ui.CancelPressed())
        {
            action = PauseMenuAction::Resumed;
        }

        ui.HoldInput();


        ui.DrawList().Rect(ui.Bounds(), kDim);

        const UIRect safe = ui.SafeArea();
        const UIRect panel = Anchor(safe, kUICenter, std::min(kWidth, safe.width), std::min(kHeight, safe.height));
        Panel(ui, panel);

        UIStack column = UIStack::Column(Inset(panel, kPadding), kGap);
        Label(ui, UIId("pause_title"), column.Next(kRow), "Paused");

        //a column that wraps
        if (Button(ui, kResume, column.Next(kRow), "Resume", {.mLinks = {kQuit, kRestart, UIId{}, UIId{}}}).mClicked)
            action = PauseMenuAction::Resumed;
        if (Button(ui, kRestart, column.Next(kRow), "Restart", {.mLinks = {kResume, kQuit, UIId{}, UIId{}}}).mClicked)
            action = PauseMenuAction::Restart;
        if (Button(ui, kQuit, column.Next(kRow), "Quit", {.mLinks = {kRestart, kResume, UIId{}, UIId{}}}).mClicked)
            action = PauseMenuAction::Quit;

        // right-aligned, so back is cut first and Select sits to its left
        UIStack prompts = UIStack::Row(column.Next(kPromptRow), kPromptGap);
        shell::DrawPrompt(ui, prompts, kUICancelAction, "Back");
        shell::DrawPrompt(ui, prompts, kUIConfirmAction, "Select");

        if (action == PauseMenuAction::Resumed || action == PauseMenuAction::Restart ||
            action == PauseMenuAction::Quit)
            mOpen = false;
        return action;
    }
} // namespace mir
