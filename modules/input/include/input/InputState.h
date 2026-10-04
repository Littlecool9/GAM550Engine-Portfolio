/**
 * @file InputState.h
 * @author Rahul Nair
 * @brief Resolved per-action input, written each frame by InputSystem and
 *        read by everything else (Lua bindings, gameplay code, editor).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputTypes.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    struct ActionValue
    {
        bool down = false;
        bool justPressed = false;
        bool justReleased = false;
        float x = 0.0f; // Axis1D's only component; Axis2D's horizontal one
        float y = 0.0f; // Axis2D only
    };

    namespace detail
    {
        struct TransparentStringHash
        {
            using is_transparent = void;
            size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
        };
    }

    class InputState
    {
    public:
        const ActionValue &Get(std::string_view action) const
        {
            static const ActionValue kZero{};
            const auto it = mActions.find(action);
            return it != mActions.end() ? it->second : kZero;
        }

        bool IsDown(std::string_view action) const { return Get(action).down; }
        bool JustPressed(std::string_view action) const { return Get(action).justPressed; }
        bool JustReleased(std::string_view action) const { return Get(action).justReleased; }
        float Axis(std::string_view action) const { return Get(action).x; }

        DeviceKind ActiveDevice() const { return mActiveDevice; }

        // Whether the editor UI wants this frame's keyboard/mouse input
        // (e.g. typing into a text field) - InputSystem checks these to
        // avoid also firing keyboard/mouse-bound actions underneath it.
        // Gamepad input is never suppressed; it can't conflict with typing.
        bool UiCapturesKeyboard() const { return mUiCapturesKeyboard; }
        bool UiCapturesMouse() const { return mUiCapturesMouse; }
        void SetUiCapture(bool keyboard, bool mouse)
        {
            mUiCapturesKeyboard = keyboard;
            mUiCapturesMouse = mouse;
        }

        // Gameplay's read: Get, but every action at rest while a game UI screen
        // holds the input, such as an open pause menu. gameui decides that
        // from its own state and writes it at the end of each of its frames.
        // The game UI goes on reading Get, so the screen holding the input is
        // still driven by every device. Not SetUiCapture, which drops a
        // device's bindings for every reader, the game UI's own actions too.
        const ActionValue &ForGame(std::string_view action) const
        {
            static const ActionValue kZero{};
            return mGameInputHeld ? kZero : Get(action);
        }

        bool GameInputHeld() const { return mGameInputHeld; }
        void SetGameInputHeld(bool held) { mGameInputHeld = held; }

        // InputSystem-only write access; everyone else reads through Get()/
        // IsDown()/etc above.
        std::unordered_map<std::string, ActionValue, detail::TransparentStringHash, std::equal_to<>> &Actions()
        {
            return mActions;
        }
        void SetActiveDevice(DeviceKind device) { mActiveDevice = device; }

    private:
        std::unordered_map<std::string, ActionValue, detail::TransparentStringHash, std::equal_to<>> mActions;
        DeviceKind mActiveDevice = DeviceKind::Keyboard;
        bool mUiCapturesKeyboard = false;
        bool mUiCapturesMouse = false;
        bool mGameInputHeld = false;
    };
}
