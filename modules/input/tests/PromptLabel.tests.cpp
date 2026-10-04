/**
 * @file PromptLabel.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for input/PromptLabel: a prompt names the action's binding on
 *        the gamepad or on the keyboard and mouse, and follows a rebind.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/DefaultInputMap.h>
#include <input/PromptLabel.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("PromptLabel names a default UI action's gamepad face button", "[input]")
{
    const InputMap map = DefaultInputMap();

    REQUIRE(PromptLabel(map, kUIConfirmAction, DeviceKind::Gamepad) == "A");
    REQUIRE(PromptLabel(map, kUICancelAction, DeviceKind::Gamepad) == "B");
    REQUIRE(PromptLabel(map, kUITabLeftAction, DeviceKind::Gamepad) == "LB");
    REQUIRE(PromptLabel(map, kUITabRightAction, DeviceKind::Gamepad) == "RB");
    REQUIRE(PromptLabel(map, kUINavigateAction, DeviceKind::Gamepad) == "LS");
}

TEST_CASE("PromptLabel names the keyboard binding under the keyboard and the mouse alike", "[input]")
{
    const InputMap map = DefaultInputMap();

    REQUIRE(PromptLabel(map, kUIConfirmAction, DeviceKind::Keyboard) == "Enter");
    REQUIRE(PromptLabel(map, kUIConfirmAction, DeviceKind::Mouse) == "Enter");
    REQUIRE(PromptLabel(map, kUICancelAction, DeviceKind::Mouse) == "Escape");
}

TEST_CASE("PromptLabel falls back to the mouse binding when there is no keyboard one", "[input]")
{
    InputMap map;
    map.AddAction("Shoot", InputActionType::Button).bindings = {
        InputBinding{.device = DeviceKind::Mouse, .mouseButton = MouseButton::Left},
        InputBinding{.device = DeviceKind::Gamepad, .gamepadButton = GamepadButton::RightBumper},
    };

    REQUIRE(PromptLabel(map, "Shoot", DeviceKind::Keyboard) == "LMB");
    REQUIRE(PromptLabel(map, "Shoot", DeviceKind::Mouse) == "LMB");
    REQUIRE(PromptLabel(map, "Shoot", DeviceKind::Gamepad) == "RB");
}

TEST_CASE("PromptLabel follows a rebind", "[input]")
{
    InputMap map = DefaultInputMap();
    InputAction *confirm = map.Find(kUIConfirmAction);
    REQUIRE(confirm != nullptr);

    for (InputBinding &binding : confirm->bindings)
    {
        if (binding.device == DeviceKind::Gamepad)
            binding.gamepadButton = GamepadButton::X;
        if (binding.device == DeviceKind::Keyboard)
            binding.key = Key::F;
    }

    REQUIRE(PromptLabel(map, kUIConfirmAction, DeviceKind::Gamepad) == "X");
    REQUIRE(PromptLabel(map, kUIConfirmAction, DeviceKind::Keyboard) == "F");
}

TEST_CASE("PromptLabel is empty for an action that is missing or unbound on that side", "[input]")
{
    InputMap map;
    map.AddAction("Pause", InputActionType::Button)
        .bindings.push_back(InputBinding{.device = DeviceKind::Keyboard, .key = Key::Escape});

    REQUIRE(PromptLabel(map, "Missing", DeviceKind::Keyboard).empty());
    REQUIRE(PromptLabel(map, "Pause", DeviceKind::Gamepad).empty());
}
