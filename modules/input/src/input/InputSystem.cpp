/**
 * @file InputSystem.cpp
 * @author Rahul Nair
 * @brief Resolves the window's RawInputSnapshot against the World's InputMap
 *        into its InputState, once per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "input/InputSystem.h"

#include <input/InputMap.h>
#include <input/InputState.h>

#include <core/ecs/World.h>
#include <window/Window.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mir
{
    namespace
    {
        constexpr float kDeviceSwitchDeadzone = 0.2f;

        float ReadBinding(const InputBinding &binding, const RawInputSnapshot &raw, bool suppressKeyboard,
                           bool suppressMouse)
        {
            switch (binding.device)
            {
            case DeviceKind::Keyboard:
                return !suppressKeyboard && raw.IsDown(binding.key) ? 1.0f : 0.0f;

            case DeviceKind::Mouse:
                return !suppressMouse && raw.IsDown(binding.mouseButton) ? 1.0f : 0.0f;

            case DeviceKind::Gamepad:
                for (const GamepadState &pad : raw.gamepads)
                {
                    if (!pad.connected)
                        continue;

                    if (binding.useGamepadAxis)
                    {
                        const float value = pad.Axis(binding.gamepadAxis);
                        return std::abs(value) < binding.deadzone ? 0.0f : value;
                    }
                    return pad.IsDown(binding.gamepadButton) ? 1.0f : 0.0f;
                }
                return 0.0f;
            }
            return 0.0f;
        }

        // scale only applies to a digital source composited into an axis
        // (e.g. D:+1/A:-1) - an analog gamepad axis is already signed.
        float ReadAxisContribution(const InputBinding &binding, const RawInputSnapshot &raw, bool suppressKeyboard,
                                    bool suppressMouse)
        {
            const float sourceValue = ReadBinding(binding, raw, suppressKeyboard, suppressMouse);
            return binding.useGamepadAxis ? sourceValue : sourceValue * binding.scale;
        }

        bool AnyKeyActive(const RawInputSnapshot &raw)
        {
            return std::any_of(raw.keys.begin(), raw.keys.end(), [](bool down)
                                { return down; });
        }

        bool AnyMouseActive(const RawInputSnapshot &raw)
        {
            if (std::any_of(raw.mouseButtons.begin(), raw.mouseButtons.end(), [](bool down)
                             { return down; }))
                return true;
            return raw.scrollX != 0.0 || raw.scrollY != 0.0;
        }

        bool AnyGamepadActive(const RawInputSnapshot &raw)
        {
            for (const GamepadState &pad : raw.gamepads)
            {
                if (!pad.connected)
                    continue;
                if (std::any_of(pad.buttons.begin(), pad.buttons.end(), [](bool down)
                                 { return down; }))
                    return true;
                for (float axis : pad.axes)
                    if (std::abs(axis) > kDeviceSwitchDeadzone)
                        return true;
            }
            return false;
        }
    }

    void InputSystem::OnUpdate(SystemContext &context)
    {
        const RawInputSnapshot &raw = mWindow.RawInput();
        const InputMap &map = context.world.Resource<InputMap>();
        InputState &state = context.world.Resource<InputState>();

        const bool suppressKeyboard = state.UiCapturesKeyboard();
        const bool suppressMouse = state.UiCapturesMouse();

        const bool mouseMoved = mHasLastMouse && (raw.mouseX != mLastMouseX || raw.mouseY != mLastMouseY);
        mLastMouseX = raw.mouseX;
        mLastMouseY = raw.mouseY;
        mHasLastMouse = true;

        if (AnyGamepadActive(raw))
            state.SetActiveDevice(DeviceKind::Gamepad);
        else if (AnyKeyActive(raw))
            state.SetActiveDevice(DeviceKind::Keyboard);
        else if (mouseMoved || AnyMouseActive(raw))
            state.SetActiveDevice(DeviceKind::Mouse);

        for (const InputAction &action : map.Actions())
        {
            ActionValue &value = state.Actions()[action.name];
            const bool wasDown = value.down;

            switch (action.type)
            {
            case InputActionType::Button:
            {
                bool down = false;
                for (const InputBinding &binding : action.bindings)
                    down = down || ReadBinding(binding, raw, suppressKeyboard, suppressMouse) != 0.0f;

                value.down = down;
                value.x = down ? 1.0f : 0.0f;
                break;
            }

            case InputActionType::Axis1D:
            {
                float total = 0.0f;
                for (const InputBinding &binding : action.bindings)
                    total += ReadAxisContribution(binding, raw, suppressKeyboard, suppressMouse);

                value.x = std::clamp(total, -1.0f, 1.0f);
                value.down = value.x != 0.0f;
                break;
            }

            case InputActionType::Axis2D:
            {
                float totalX = 0.0f;
                float totalY = 0.0f;
                for (const InputBinding &binding : action.bindings)
                {
                    float &target = binding.channel == AxisChannel::Y ? totalY : totalX;
                    target += ReadAxisContribution(binding, raw, suppressKeyboard, suppressMouse);
                }

                value.x = std::clamp(totalX, -1.0f, 1.0f);
                value.y = std::clamp(totalY, -1.0f, 1.0f);
                value.down = value.x != 0.0f || value.y != 0.0f;
                break;
            }
            }

            value.justPressed = value.down && !wasDown;
            value.justReleased = !value.down && wasDown;
        }

        std::erase_if(state.Actions(), [&](const auto &entry)
                      { return map.Find(entry.first) == nullptr; });
    }
}
