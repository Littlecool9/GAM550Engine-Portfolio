/**
 * @file PromptLabel.cpp
 * @author Quy Dong Ta
 * @brief The short label a button prompt shows for an action, taken from the
 *        action's own bindings so a rebind in Project Settings changes it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "input/PromptLabel.h"

namespace mir
{
    namespace
    {
        std::string_view MouseLabel(MouseButton button)
        {
            switch (button)
            {
            case MouseButton::Left:
                return "LMB";
            case MouseButton::Right:
                return "RMB";
            case MouseButton::Middle:
                return "MMB";
            case MouseButton::Button4:
                return "Mouse4";
            case MouseButton::Button5:
                return "Mouse5";
            default:
                return MouseButtonName(button);
            }
        }

        std::string_view GamepadButtonLabel(GamepadButton button)
        {
            switch (button)
            {
            case GamepadButton::LeftBumper:
                return "LB";
            case GamepadButton::RightBumper:
                return "RB";
            case GamepadButton::LeftThumb:
                return "LS";
            case GamepadButton::RightThumb:
                return "RS";
            case GamepadButton::DpadUp:
            case GamepadButton::DpadRight:
            case GamepadButton::DpadDown:
            case GamepadButton::DpadLeft:
                return "D-Pad";
            default:
                return GamepadButtonName(button);
            }
        }

        std::string_view GamepadAxisLabel(GamepadAxis axis)
        {
            switch (axis)
            {
            case GamepadAxis::LeftX:
            case GamepadAxis::LeftY:
                return "LS";
            case GamepadAxis::RightX:
            case GamepadAxis::RightY:
                return "RS";
            case GamepadAxis::LeftTrigger:
                return "LT";
            case GamepadAxis::RightTrigger:
                return "RT";
            }
            return GamepadAxisName(axis);
        }

        const InputBinding *FirstOn(const InputAction &action, DeviceKind device)
        {
            for (const InputBinding &binding : action.bindings)
                if (binding.device == device)
                    return &binding;
            return nullptr;
        }
    }

    std::string_view PromptLabel(const InputMap &map, std::string_view action, DeviceKind device)
    {
        const InputAction *found = map.Find(action);
        if (found == nullptr)
            return {};

        if (device == DeviceKind::Gamepad)
        {
            const InputBinding *binding = FirstOn(*found, DeviceKind::Gamepad);
            if (binding == nullptr)
                return {};
            return binding->useGamepadAxis ? GamepadAxisLabel(binding->gamepadAxis)
                                           : GamepadButtonLabel(binding->gamepadButton);
        }

        if (const InputBinding *binding = FirstOn(*found, DeviceKind::Keyboard))
            return KeyName(binding->key);
        if (const InputBinding *binding = FirstOn(*found, DeviceKind::Mouse))
            return MouseLabel(binding->mouseButton);
        return {};
    }
}
