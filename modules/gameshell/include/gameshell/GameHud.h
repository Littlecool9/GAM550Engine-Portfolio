/**
 * @file GameHud.h
 * @author Quy Dong Ta
 * @brief The HUD over a game in play: the play time, and a pause button that
 *        shows the pause binding for the active device and opens the pause
 *        menu when the pointer clicks it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIContext.h>

namespace mir
{

    class GameHud
    {
    public:

        bool Draw(UIContext &ui, float playSeconds, bool interactive);
    };
} // namespace mir
