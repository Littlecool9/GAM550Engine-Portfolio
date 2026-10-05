/**
 * @file DefaultInputMap.h
 * @author Quy Dong Ta
 * @brief The actions every project ships with, and their keyboard, mouse and
 *        gamepad bindings. The UI actions live here so device differences are
 *        settled by bindings, and gameui reads them by name alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "InputMap.h"

#include <cstddef>
#include <string_view>

namespace mir
{
    inline constexpr std::string_view kUINavigateAction = "UINavigate";
    inline constexpr std::string_view kUIConfirmAction = "UIConfirm";
    inline constexpr std::string_view kUICancelAction = "UICancel";
    inline constexpr std::string_view kUITabLeftAction = "UITabLeft";
    inline constexpr std::string_view kUITabRightAction = "UITabRight";

    inline constexpr std::string_view kUIPauseAction = "UIPause";

    inline constexpr std::string_view kToggleFullscreenAction = "ToggleFullscreen";

    InputMap DefaultInputMap();


    size_t AddDefaultActions(InputMap &map);
}
