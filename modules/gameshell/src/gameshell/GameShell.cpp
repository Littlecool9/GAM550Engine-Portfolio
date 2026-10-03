/**
 * @file GameShell.cpp
 * @author Quy Dong Ta
 * @brief The screens every game gets: the HUD while it plays and the pause
 *        menu over it, with what the player asked of the game this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "gameshell/GameShell.h"

namespace mir
{
    GameShellAction GameShell::Draw(UIContext &ui, bool playing, float dt)
    {
        if (!playing)
        {
            mWasPlaying = false;
            mMenu.Draw(ui, false);
            return GameShellAction::None;
        }

        if (!mWasPlaying)
            mPlaySeconds = 0.0f;
        mWasPlaying = true;
        if (!mMenu.IsOpen())
            mPlaySeconds += dt;

        if (mHud.Draw(ui, mPlaySeconds, !mMenu.IsOpen()))
            mMenu.Open();

        switch (mMenu.Draw(ui, true))
        {
        case PauseMenuAction::Opened:
            return GameShellAction::Pause;
        case PauseMenuAction::Resumed:
            return GameShellAction::Resume;
        case PauseMenuAction::Restart:
            mPlaySeconds = 0.0f;
            return GameShellAction::Restart;
        case PauseMenuAction::Quit:
            return GameShellAction::Quit;
        case PauseMenuAction::None:
            break;
        }
        return GameShellAction::None;
    }
} // namespace mir
