/**
 * @file DefaultInputMap.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for input/DefaultInputMap: the UI actions, their bindings on
 *        each device, the save/load round trip, and merging into a loaded map;
 *        and InputState's ForGame while a game UI holds the input.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>
#include <window/Window.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

using namespace mir;

namespace
{
    constexpr std::string_view kUIActions[] = {
        kUINavigateAction, kUIConfirmAction, kUICancelAction, kUITabLeftAction, kUITabRightAction, kUIPauseAction,
    };

    // the mouse navigates by pointing and pauses by clicking the HUD, so these
    // two have no mouse binding
    bool BindsMouse(std::string_view action) { return action != kUINavigateAction && action != kUIPauseAction; }

    bool BindsDevice(const InputAction &action, DeviceKind device)
    {
        return std::any_of(action.bindings.begin(), action.bindings.end(),
                           [&](const InputBinding &binding) { return binding.device == device; });
    }

    bool SameBinding(const InputBinding &a, const InputBinding &b)
    {
        return a.device == b.device && a.channel == b.channel && a.key == b.key && a.mouseButton == b.mouseButton &&
               a.useGamepadAxis == b.useGamepadAxis && a.gamepadButton == b.gamepadButton &&
               a.gamepadAxis == b.gamepadAxis && a.deadzone == b.deadzone && a.scale == b.scale;
    }

    struct TempSettingsFile
    {
        std::filesystem::path mPath;

        explicit TempSettingsFile(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_input_tests" / name)
        {
            std::filesystem::create_directories(mPath.parent_path());
            std::filesystem::remove(mPath);
        }

        ~TempSettingsFile() { std::filesystem::remove(mPath); }
    };

    class TestWindow final : public Window
    {
    public:
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        NativeWindowHandle NativeWindow() const override { return {}; }
        uint32_t Width() const override { return 1; }
        uint32_t Height() const override { return 1; }

        const RawInputSnapshot &RawInput() const override { return mRaw; }

        RawInputSnapshot mRaw;
    };

    struct Fixture
    {
        TestWindow window;
        World world;
        CommandBuffer commands;
        InputMap &map = world.EmplaceResource<InputMap>(DefaultInputMap());
        InputState &state = world.EmplaceResource<InputState>();
        InputSystem system{window};
        SystemContext context{world, commands, 0.016f, 0.0, 0};

        void Update() { system.OnUpdate(context); }
        void SetKey(Key key, bool down) { window.mRaw.keys[static_cast<size_t>(key)] = down; }
        void SetMouse(MouseButton button, bool down) { window.mRaw.mouseButtons[static_cast<size_t>(button)] = down; }
        GamepadState &Pad()
        {
            window.mRaw.gamepads[0].connected = true;
            return window.mRaw.gamepads[0];
        }
    };
} // namespace

TEST_CASE("The default map has the six UI actions with their types", "[input]")
{
    const InputMap map = DefaultInputMap();

    for (std::string_view name : kUIActions)
    {
        INFO(name);
        const InputAction *action = map.Find(name);
        REQUIRE(action != nullptr);
        CHECK(action->type == (name == kUINavigateAction ? InputActionType::Axis2D : InputActionType::Button));
    }
}

TEST_CASE("Every UI action is bound on keyboard and gamepad, and all but UINavigate and UIPause on the mouse",
          "[input]")
{
    const InputMap map = DefaultInputMap();

    for (std::string_view name : kUIActions)
    {
        INFO(name);
        const InputAction &action = *map.Find(name);
        CHECK(BindsDevice(action, DeviceKind::Keyboard));
        CHECK(BindsDevice(action, DeviceKind::Gamepad));
        CHECK(BindsDevice(action, DeviceKind::Mouse) == BindsMouse(name));
    }
}

TEST_CASE("The default map round-trips through SaveFile and LoadFile unchanged", "[input]")
{
    TempSettingsFile file{"default_round_trip.json"};
    const InputMap saved = DefaultInputMap();

    REQUIRE(saved.SaveFile(file.mPath));
    const std::optional<InputMap> loaded = InputMap::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->Actions().size() == saved.Actions().size());
    for (size_t i = 0; i < saved.Actions().size(); ++i)
    {
        const InputAction &expected = saved.Actions()[i];
        const InputAction &actual = loaded->Actions()[i];
        INFO(expected.name);
        CHECK(actual.name == expected.name);
        CHECK(actual.type == expected.type);
        REQUIRE(actual.bindings.size() == expected.bindings.size());
        for (size_t b = 0; b < expected.bindings.size(); ++b)
            CHECK(SameBinding(actual.bindings[b], expected.bindings[b]));
    }
}

TEST_CASE("AddDefaultActions fills an empty map and then adds nothing more", "[input]")
{
    InputMap map;

    CHECK(AddDefaultActions(map) == std::size(kUIActions));
    CHECK(map.Actions().size() == std::size(kUIActions));
    CHECK(AddDefaultActions(map) == 0);
    CHECK(map.Actions().size() == std::size(kUIActions));
}

TEST_CASE("AddDefaultActions keeps a rebound default and the project's own actions", "[input]")
{
    InputMap map;
    map.AddAction("Jump", InputActionType::Button);
    map.AddAction(std::string(kUIConfirmAction), InputActionType::Button)
        .bindings.push_back(InputBinding{.device = DeviceKind::Keyboard, .key = Key::F});

    CHECK(AddDefaultActions(map) == std::size(kUIActions) - 1);

    const InputAction &confirm = *map.Find(kUIConfirmAction);
    REQUIRE(confirm.bindings.size() == 1);
    CHECK(confirm.bindings[0].key == Key::F);
    CHECK(map.Find("Jump") != nullptr);
}

TEST_CASE("UINavigate resolves in UI space from keys, the D-pad and the stick", "[input]")
{
    Fixture f;

    f.SetKey(Key::Down, true);
    f.SetKey(Key::Left, true);
    f.Update();
    CHECK(f.state.Get(kUINavigateAction).x == -1.0f);
    CHECK(f.state.Get(kUINavigateAction).y == 1.0f);
    CHECK(f.state.JustPressed(kUINavigateAction));
    f.SetKey(Key::Down, false);
    f.SetKey(Key::Left, false);

    f.Pad().buttons[static_cast<size_t>(GamepadButton::DpadUp)] = true;
    f.Update();
    CHECK(f.state.Get(kUINavigateAction).y == -1.0f);
    f.Pad().buttons[static_cast<size_t>(GamepadButton::DpadUp)] = false;

    f.Pad().axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.5f;
    f.Pad().axes[static_cast<size_t>(GamepadAxis::LeftY)] = 0.75f;
    f.Update();
    CHECK(f.state.Get(kUINavigateAction).x == 0.5f);
    CHECK(f.state.Get(kUINavigateAction).y == 0.75f);
}

TEST_CASE("UI buttons resolve the same from each device", "[input]")
{
    Fixture f;

    f.SetKey(Key::Enter, true);
    f.Update();
    CHECK(f.state.JustPressed(kUIConfirmAction));
    f.SetKey(Key::Enter, false);
    f.Update();

    f.SetMouse(MouseButton::Left, true);
    f.Update();
    CHECK(f.state.JustPressed(kUIConfirmAction));
    f.SetMouse(MouseButton::Left, false);

    f.Pad().buttons[static_cast<size_t>(GamepadButton::B)] = true;
    f.Pad().buttons[static_cast<size_t>(GamepadButton::RightBumper)] = true;
    f.Update();
    CHECK(f.state.JustPressed(kUICancelAction));
    CHECK(f.state.JustPressed(kUITabRightAction));
    CHECK_FALSE(f.state.IsDown(kUITabLeftAction));
}

TEST_CASE("UIPause comes from Escape and the gamepad's Start", "[input]")
{
    Fixture f;

    f.SetKey(Key::Escape, true);
    f.Update();
    CHECK(f.state.JustPressed(kUIPauseAction));
    // the same key backs out of a menu
    CHECK(f.state.JustPressed(kUICancelAction));
    f.SetKey(Key::Escape, false);
    f.Update();

    f.Pad().buttons[static_cast<size_t>(GamepadButton::Start)] = true;
    f.Update();
    CHECK(f.state.JustPressed(kUIPauseAction));
    CHECK_FALSE(f.state.IsDown(kUICancelAction));
}

TEST_CASE("ForGame reads every action at rest while a game UI holds the input, and Get does not", "[input]")
{
    Fixture f;
    f.map.AddAction("Jump", InputActionType::Button)
        .bindings.push_back(InputBinding{.device = DeviceKind::Keyboard, .key = Key::Space});

    f.SetKey(Key::Space, true);
    f.SetKey(Key::Down, true);
    f.Update();
    CHECK(f.state.ForGame("Jump").down);
    CHECK(f.state.ForGame("Jump").justPressed);

    f.state.SetGameInputHeld(true);
    CHECK_FALSE(f.state.ForGame("Jump").down);
    CHECK_FALSE(f.state.ForGame("Jump").justPressed);
    CHECK(f.state.ForGame(kUINavigateAction).y == 0.0f);

    // the game UI's own reads, and every device, are untouched
    CHECK(f.state.Get("Jump").down);
    CHECK(f.state.Get(kUINavigateAction).y == 1.0f);
    CHECK_FALSE(f.state.UiCapturesKeyboard());
    CHECK_FALSE(f.state.UiCapturesMouse());

    f.state.SetGameInputHeld(false);
    CHECK(f.state.ForGame("Jump").down);
}
