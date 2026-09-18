/**
 * @file InputCodes.cpp
 * @author Rahul Nair
 * @brief Backend-neutral key/button/axis identifiers for the raw input layer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "window/InputCodes.h"

#include <array>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        constexpr std::pair<Key, std::string_view> kKeyNames[] = {
            {Key::Space, "Space"}, {Key::Apostrophe, "Apostrophe"}, {Key::Comma, "Comma"},
            {Key::Minus, "Minus"}, {Key::Period, "Period"}, {Key::Slash, "Slash"},
            {Key::Num0, "Num0"}, {Key::Num1, "Num1"}, {Key::Num2, "Num2"}, {Key::Num3, "Num3"},
            {Key::Num4, "Num4"}, {Key::Num5, "Num5"}, {Key::Num6, "Num6"}, {Key::Num7, "Num7"},
            {Key::Num8, "Num8"}, {Key::Num9, "Num9"},
            {Key::Semicolon, "Semicolon"}, {Key::Equal, "Equal"},
            {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"}, {Key::F, "F"},
            {Key::G, "G"}, {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"},
            {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"}, {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"},
            {Key::S, "S"}, {Key::T, "T"}, {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"},
            {Key::Y, "Y"}, {Key::Z, "Z"},
            {Key::LeftBracket, "LeftBracket"}, {Key::Backslash, "Backslash"},
            {Key::RightBracket, "RightBracket"}, {Key::GraveAccent, "GraveAccent"},
            {Key::Escape, "Escape"}, {Key::Enter, "Enter"}, {Key::Tab, "Tab"},
            {Key::Backspace, "Backspace"}, {Key::Insert, "Insert"}, {Key::Delete, "Delete"},
            {Key::Right, "Right"}, {Key::Left, "Left"}, {Key::Down, "Down"}, {Key::Up, "Up"},
            {Key::PageUp, "PageUp"}, {Key::PageDown, "PageDown"}, {Key::Home, "Home"}, {Key::End, "End"},
            {Key::CapsLock, "CapsLock"}, {Key::ScrollLock, "ScrollLock"}, {Key::NumLock, "NumLock"},
            {Key::PrintScreen, "PrintScreen"}, {Key::Pause, "Pause"},
            {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"}, {Key::F5, "F5"},
            {Key::F6, "F6"}, {Key::F7, "F7"}, {Key::F8, "F8"}, {Key::F9, "F9"}, {Key::F10, "F10"},
            {Key::F11, "F11"}, {Key::F12, "F12"}, {Key::F13, "F13"}, {Key::F14, "F14"}, {Key::F15, "F15"},
            {Key::F16, "F16"}, {Key::F17, "F17"}, {Key::F18, "F18"}, {Key::F19, "F19"}, {Key::F20, "F20"},
            {Key::F21, "F21"}, {Key::F22, "F22"}, {Key::F23, "F23"}, {Key::F24, "F24"}, {Key::F25, "F25"},
            {Key::Keypad0, "Keypad0"}, {Key::Keypad1, "Keypad1"}, {Key::Keypad2, "Keypad2"},
            {Key::Keypad3, "Keypad3"}, {Key::Keypad4, "Keypad4"}, {Key::Keypad5, "Keypad5"},
            {Key::Keypad6, "Keypad6"}, {Key::Keypad7, "Keypad7"}, {Key::Keypad8, "Keypad8"},
            {Key::Keypad9, "Keypad9"}, {Key::KeypadDecimal, "KeypadDecimal"},
            {Key::KeypadDivide, "KeypadDivide"}, {Key::KeypadMultiply, "KeypadMultiply"},
            {Key::KeypadSubtract, "KeypadSubtract"}, {Key::KeypadAdd, "KeypadAdd"},
            {Key::KeypadEnter, "KeypadEnter"}, {Key::KeypadEqual, "KeypadEqual"},
            {Key::LeftShift, "LeftShift"}, {Key::LeftControl, "LeftControl"}, {Key::LeftAlt, "LeftAlt"},
            {Key::LeftSuper, "LeftSuper"}, {Key::RightShift, "RightShift"},
            {Key::RightControl, "RightControl"}, {Key::RightAlt, "RightAlt"},
            {Key::RightSuper, "RightSuper"}, {Key::Menu, "Menu"},
        };

        constexpr std::pair<MouseButton, std::string_view> kMouseButtonNames[] = {
            {MouseButton::Left, "Left"}, {MouseButton::Right, "Right"}, {MouseButton::Middle, "Middle"},
            {MouseButton::Button4, "Button4"}, {MouseButton::Button5, "Button5"},
            {MouseButton::Button6, "Button6"}, {MouseButton::Button7, "Button7"},
            {MouseButton::Button8, "Button8"},
        };

        // Canonical names only (Cross/Circle/Square/Triangle are aliases of
        // A/B/X/Y with the same underlying value - they round-trip as A/B/X/Y).
        constexpr std::pair<GamepadButton, std::string_view> kGamepadButtonNames[] = {
            {GamepadButton::A, "A"}, {GamepadButton::B, "B"}, {GamepadButton::X, "X"}, {GamepadButton::Y, "Y"},
            {GamepadButton::LeftBumper, "LeftBumper"}, {GamepadButton::RightBumper, "RightBumper"},
            {GamepadButton::Back, "Back"}, {GamepadButton::Start, "Start"}, {GamepadButton::Guide, "Guide"},
            {GamepadButton::LeftThumb, "LeftThumb"}, {GamepadButton::RightThumb, "RightThumb"},
            {GamepadButton::DpadUp, "DpadUp"}, {GamepadButton::DpadRight, "DpadRight"},
            {GamepadButton::DpadDown, "DpadDown"}, {GamepadButton::DpadLeft, "DpadLeft"},
        };

        constexpr std::pair<GamepadAxis, std::string_view> kGamepadAxisNames[] = {
            {GamepadAxis::LeftX, "LeftX"}, {GamepadAxis::LeftY, "LeftY"},
            {GamepadAxis::RightX, "RightX"}, {GamepadAxis::RightY, "RightY"},
            {GamepadAxis::LeftTrigger, "LeftTrigger"}, {GamepadAxis::RightTrigger, "RightTrigger"},
        };

        template <typename Enum, std::size_t N>
        std::string_view NameOf(const std::pair<Enum, std::string_view> (&table)[N], Enum value)
        {
            for (const auto &[candidate, name] : table)
                if (candidate == value)
                    return name;
            return {};
        }

        template <typename Enum, std::size_t N>
        std::optional<Enum> ParseOf(const std::pair<Enum, std::string_view> (&table)[N], std::string_view name)
        {
            for (const auto &[value, candidate] : table)
                if (candidate == name)
                    return value;
            return std::nullopt;
        }
    }

    std::string_view KeyName(Key key) { return NameOf(kKeyNames, key); }
    Key ParseKey(std::string_view name) { return ParseOf(kKeyNames, name).value_or(Key::Unknown); }

    std::span<const Key> AllKeys()
    {
        static const std::vector<Key> all = [] {
            std::vector<Key> keys;
            keys.reserve(std::size(kKeyNames));
            for (const auto &[key, name] : kKeyNames)
                keys.push_back(key);
            return keys;
        }();
        return all;
    }

    std::string_view MouseButtonName(MouseButton button) { return NameOf(kMouseButtonNames, button); }
    std::optional<MouseButton> ParseMouseButton(std::string_view name) { return ParseOf(kMouseButtonNames, name); }

    std::string_view GamepadButtonName(GamepadButton button) { return NameOf(kGamepadButtonNames, button); }
    std::optional<GamepadButton> ParseGamepadButton(std::string_view name) { return ParseOf(kGamepadButtonNames, name); }

    std::string_view GamepadAxisName(GamepadAxis axis) { return NameOf(kGamepadAxisNames, axis); }
    std::optional<GamepadAxis> ParseGamepadAxis(std::string_view name) { return ParseOf(kGamepadAxisNames, name); }
}
