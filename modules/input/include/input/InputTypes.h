/**
 * @file InputTypes.h
 * @author Rahul Nair
 * @brief The data model an InputMap is made of: actions and their bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <window/InputCodes.h>

#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class DeviceKind
    {
        Keyboard,
        Mouse,
        Gamepad,
    };

    enum class InputActionType
    {
        Button,
        Axis1D,
        Axis2D,
    };

    enum class AxisChannel
    {
        X,
        Y,
    };

    struct InputBinding
    {
        DeviceKind device = DeviceKind::Keyboard;
        AxisChannel channel = AxisChannel::X;

        Key key = Key::Unknown;
        MouseButton mouseButton = MouseButton::Left;

        bool useGamepadAxis = false;
        GamepadButton gamepadButton = GamepadButton::A;
        GamepadAxis gamepadAxis = GamepadAxis::LeftX;
        float deadzone = 0.15f; // only applies when useGamepadAxis is true
        float scale = 1.0f;

        bool operator==(const InputBinding &) const = default;
    };

    struct InputAction
    {
        std::string name;
        InputActionType type = InputActionType::Button;
        std::vector<InputBinding> bindings;

        bool operator==(const InputAction &) const = default;
    };

    std::string_view ActionTypeName(InputActionType type);
    InputActionType ParseActionType(std::string_view name); // unrecognised name -> Button
}
