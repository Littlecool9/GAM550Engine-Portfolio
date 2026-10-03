/**
 * @file PauseMenu.h
 * @author Quy Dong Ta
 * @brief The menu a game pauses into: Resume, Restart and Quit over a dimmed
 *        screen, driven by gamepad, mouse or keyboard, built on gameui's
 *        public API alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIContext.h>

#include <cstdint>

namespace mir
{

    enum class PauseMenuAction : uint8_t
    {
        None,
        Opened,
        Resumed,
        Restart,
        Quit,
    };

    class PauseMenu
    {
    public:

        PauseMenuAction Draw(UIContext &ui, bool canOpen);

        void Open() { mOpenRequested = true; }

        bool IsOpen() const { return mOpen; }

    private:
        bool mOpen = false;
        bool mOpenRequested = false;
    };
} // namespace mir
