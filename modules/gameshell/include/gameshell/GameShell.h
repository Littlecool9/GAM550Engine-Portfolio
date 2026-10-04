/**
 * @file GameShell.h
 * @author Quy Dong Ta
 * @brief The screens every game gets: the HUD while it plays and the pause
 *        menu over it, with what the player asked of the game this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "GameHud.h"
#include "PauseMenu.h"

#include <gameui/UIContext.h>

#include <cstdint>

namespace mir
{
    enum class GameShellAction : uint8_t
    {
        None,
        Pause,
        Resume,
        Restart,
        Quit,
    };

    class GameShell
    {
    public:

        GameShellAction Draw(UIContext &ui, bool playing, float dt);

        bool MenuOpen() const { return mMenu.IsOpen(); }

        float PlaySeconds() const { return mPlaySeconds; }

    private:
        GameHud mHud;
        PauseMenu mMenu;
        float mPlaySeconds = 0.0f;
        bool mWasPlaying = false;
    };
} // namespace mir
