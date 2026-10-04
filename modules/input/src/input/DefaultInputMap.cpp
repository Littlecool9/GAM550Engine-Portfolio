/**
 * @file DefaultInputMap.cpp
 * @author Quy Dong Ta
 * @brief The actions every project ships with, and their keyboard, mouse and
 *        gamepad bindings. The UI actions live here so device differences are
 *        settled by bindings, and gameui reads them by name alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "input/DefaultInputMap.h"

#include <string>

namespace mir
{
    namespace
    {
        InputBinding KeyBinding(Key key, AxisChannel channel = AxisChannel::X, float scale = 1.0f)
        {
            return InputBinding{ .device = DeviceKind::Keyboard, .channel = channel, .key = key, .scale = scale };
        }

        InputBinding MouseBinding(MouseButton button)
        {
            return InputBinding{ .device = DeviceKind::Mouse, .mouseButton = button };
        }

        InputBinding PadButtonBinding(GamepadButton button, AxisChannel channel = AxisChannel::X, float scale = 1.0f)
        {
            return InputBinding{ .device = DeviceKind::Gamepad, .channel = channel, .gamepadButton = button, .scale = scale };
        }

        InputBinding PadAxisBinding(GamepadAxis axis, AxisChannel channel)
        {
            return InputBinding{ .device = DeviceKind::Gamepad, .channel = channel, .useGamepadAxis = true, .gamepadAxis = axis };
        }

        void AddUIActions(InputMap &map)
        {

            InputAction &navigate = map.AddAction(std::string(kUINavigateAction), InputActionType::Axis2D);
            navigate.bindings = {
                KeyBinding(Key::Left, AxisChannel::X, -1.0f),
                KeyBinding(Key::Right, AxisChannel::X, 1.0f),
                KeyBinding(Key::Up, AxisChannel::Y, -1.0f),
                KeyBinding(Key::Down, AxisChannel::Y, 1.0f),
                PadAxisBinding(GamepadAxis::LeftX, AxisChannel::X),
                PadAxisBinding(GamepadAxis::LeftY, AxisChannel::Y),
                PadButtonBinding(GamepadButton::DpadLeft, AxisChannel::X, -1.0f),
                PadButtonBinding(GamepadButton::DpadRight, AxisChannel::X, 1.0f),
                PadButtonBinding(GamepadButton::DpadUp, AxisChannel::Y, -1.0f),
                PadButtonBinding(GamepadButton::DpadDown, AxisChannel::Y, 1.0f),
            };

            map.AddAction(std::string(kUIConfirmAction), InputActionType::Button).bindings = {
                KeyBinding(Key::Enter),
                KeyBinding(Key::Space),
                MouseBinding(MouseButton::Left),
                PadButtonBinding(GamepadButton::A),
            };

            map.AddAction(std::string(kUICancelAction), InputActionType::Button).bindings = {
                KeyBinding(Key::Escape),
                MouseBinding(MouseButton::Right),
                PadButtonBinding(GamepadButton::B),
            };

            //button4 n 5 = mouse forward/back buttons
            map.AddAction(std::string(kUITabLeftAction), InputActionType::Button).bindings = {
                KeyBinding(Key::Q),
                MouseBinding(MouseButton::Button4),
                PadButtonBinding(GamepadButton::LeftBumper),
            };

            map.AddAction(std::string(kUITabRightAction), InputActionType::Button).bindings = {
                KeyBinding(Key::E),
                MouseBinding(MouseButton::Button5),
                PadButtonBinding(GamepadButton::RightBumper),
            };

            map.AddAction(std::string(kUIPauseAction), InputActionType::Button).bindings = {
                KeyBinding(Key::Escape),
                PadButtonBinding(GamepadButton::Start),
            };
        }
    }

    InputMap DefaultInputMap()
    {
        InputMap map;
        AddUIActions(map);
        return map;
    }

    size_t AddDefaultActions(InputMap &map)
    {
        const InputMap defaults = DefaultInputMap();

        size_t added = 0;
        for (const InputAction &action : defaults.Actions())
        {
            if (map.Find(action.name) != nullptr)
                continue;

            map.MutableActions().push_back(action);
            ++added;
        }
        return added;
    }
}
