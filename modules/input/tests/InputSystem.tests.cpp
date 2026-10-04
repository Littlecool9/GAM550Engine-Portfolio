/**
 * @file InputSystem.tests.cpp
 * @author Rahul Nair
 * @brief Tests for input/InputSystem - resolution logic, device switching,
 *        UI-capture suppression, and the stale-removed-action pruning fix.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>
#include <window/Window.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class TestWindow final : public mir::Window
    {
    public:
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        mir::NativeWindowHandle NativeWindow() const override { return {}; }
        uint32_t Width() const override { return 1; }
        uint32_t Height() const override { return 1; }

        const mir::RawInputSnapshot &RawInput() const override { return mRaw; }

        mir::RawInputSnapshot mRaw;
    };

    struct Fixture
    {
        TestWindow window;
        mir::World world;
        mir::CommandBuffer commands;
        mir::InputMap &map = world.EmplaceResource<mir::InputMap>();
        mir::InputState &state = world.EmplaceResource<mir::InputState>();
        mir::InputSystem system{window};
        mir::SystemContext context{world, commands, 0.016f, 0.0, 0};

        void Update() { system.OnUpdate(context); }
    };
}

TEST_CASE("Button action is down when any binding is down, with edge detection", "[input]")
{
    Fixture f;
    f.map.AddAction("Jump", mir::InputActionType::Button)
        .bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::Space});

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.Update();
    REQUIRE(f.state.IsDown("Jump"));
    REQUIRE(f.state.JustPressed("Jump"));

    f.Update(); // held, not a fresh press
    REQUIRE(f.state.IsDown("Jump"));
    REQUIRE_FALSE(f.state.JustPressed("Jump"));

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = false;
    f.Update();
    REQUIRE_FALSE(f.state.IsDown("Jump"));
    REQUIRE(f.state.JustReleased("Jump"));
}

TEST_CASE("Axis1D composes digital bindings via scale and clamps to [-1,1]", "[input]")
{
    Fixture f;
    mir::InputAction &move = f.map.AddAction("Move", mir::InputActionType::Axis1D);
    move.bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::D, .scale = 1.0f});
    move.bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::A, .scale = -1.0f});

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::D)] = true;
    f.Update();
    REQUIRE(f.state.Axis("Move") == 1.0f);

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::D)] = false;
    f.window.mRaw.keys[static_cast<size_t>(mir::Key::A)] = true;
    f.Update();
    REQUIRE(f.state.Axis("Move") == -1.0f);
}

TEST_CASE("Axis2D routes each binding's channel into x and y independently", "[input]")
{
    Fixture f;
    mir::InputAction &look = f.map.AddAction("Look", mir::InputActionType::Axis2D);
    look.bindings.push_back(mir::InputBinding{
        .device = mir::DeviceKind::Keyboard, .channel = mir::AxisChannel::X, .key = mir::Key::Right, .scale = 1.0f});
    look.bindings.push_back(mir::InputBinding{
        .device = mir::DeviceKind::Keyboard, .channel = mir::AxisChannel::Y, .key = mir::Key::Up, .scale = 1.0f});

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Right)] = true;
    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Up)] = true;
    f.Update();

    REQUIRE(f.state.Get("Look").x == 1.0f);
    REQUIRE(f.state.Get("Look").y == 1.0f);
}

TEST_CASE("Active device is Keyboard when a key is down", "[input]")
{
    Fixture f;
    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Keyboard);
}

TEST_CASE("Active device is Mouse when only mouse input occurs - not stuck on Keyboard", "[input]")
{
    Fixture f;
    f.window.mRaw.mouseButtons[static_cast<size_t>(mir::MouseButton::Left)] = true;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Mouse);
}

TEST_CASE("Active device is Mouse when the cursor moves with no button down", "[input]")
{
    Fixture f;
    f.window.mRaw.gamepads[0].connected = true;
    f.window.mRaw.gamepads[0].buttons[static_cast<size_t>(mir::GamepadButton::A)] = true;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Gamepad);

    f.window.mRaw.gamepads[0].buttons[static_cast<size_t>(mir::GamepadButton::A)] = false;
    f.Update(); // a cursor that has not moved is not mouse input
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Gamepad);

    f.window.mRaw.mouseX = 10.0;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Mouse);
}

TEST_CASE("The cursor's first position is not a move", "[input]")
{
    Fixture f;
    f.window.mRaw.mouseX = 300.0;
    f.window.mRaw.mouseY = 200.0;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Keyboard);
}

TEST_CASE("Gamepad activity takes priority over keyboard for active device", "[input]")
{
    Fixture f;
    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.window.mRaw.gamepads[0].connected = true;
    f.window.mRaw.gamepads[0].buttons[static_cast<size_t>(mir::GamepadButton::A)] = true;
    f.Update();
    REQUIRE(f.state.ActiveDevice() == mir::DeviceKind::Gamepad);
}

TEST_CASE("Removing an action from the map prunes it from state instead of freezing its value", "[input]")
{
    Fixture f;
    f.map.AddAction("Jump", mir::InputActionType::Button)
        .bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::Space});

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.Update();
    REQUIRE(f.state.IsDown("Jump"));

    f.map.RemoveAction("Jump");
    f.Update();

    REQUIRE_FALSE(f.state.IsDown("Jump")); // pruned, not stuck at true
}

TEST_CASE("UiCapturesKeyboard suppresses a keyboard binding but not a gamepad one on the same action", "[input]")
{
    Fixture f;
    mir::InputAction &jump = f.map.AddAction("Jump", mir::InputActionType::Button);
    jump.bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::Space});
    mir::InputBinding padBinding{.device = mir::DeviceKind::Gamepad};
    padBinding.gamepadButton = mir::GamepadButton::A;
    jump.bindings.push_back(padBinding);

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.window.mRaw.gamepads[0].connected = true;
    f.window.mRaw.gamepads[0].buttons[static_cast<size_t>(mir::GamepadButton::A)] = true;
    f.state.SetUiCapture(/*keyboard*/ true, /*mouse*/ false);

    f.Update();

    REQUIRE(f.state.IsDown("Jump")); // still true via the (unsuppressed) gamepad binding
}

TEST_CASE("UiCapturesKeyboard makes a keyboard-only action read as not down", "[input]")
{
    Fixture f;
    f.map.AddAction("Jump", mir::InputActionType::Button)
        .bindings.push_back(mir::InputBinding{.device = mir::DeviceKind::Keyboard, .key = mir::Key::Space});

    f.window.mRaw.keys[static_cast<size_t>(mir::Key::Space)] = true;
    f.state.SetUiCapture(/*keyboard*/ true, /*mouse*/ false);

    f.Update();

    REQUIRE_FALSE(f.state.IsDown("Jump"));
}
