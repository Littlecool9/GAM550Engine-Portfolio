/**
 * @file PromptLabel.h
 * @author Quy Dong Ta
 * @brief The short label a button prompt shows for an action, taken from the
 *        action's own bindings so a rebind in Project Settings changes it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "InputMap.h"

#include <string_view>

namespace mir
{
    // Gamepad: the action's first gamepad binding, e.g. "A" or "LB".
    // Keyboard or Mouse: its first keyboard binding, e.g. "Enter", or failing
    // that its first mouse binding, e.g. "LMB" - one keyboard and mouse side.
    // Empty when the action is not in the map or has no binding on that side.
    std::string_view PromptLabel(const InputMap &map, std::string_view action, DeviceKind device);
}
