/**
 * @file UIActiveDevice.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for UIContext against the input: the two things gameui decides
 *        by the active device, whether the focus highlight is drawn and which
 *        label a button prompt shows, and a screen holding the input.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>

#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

using namespace mir;

namespace
{
    // an empty area so EndFrame has nothing to hand the ImGui backend
    constexpr UIRect kNoArea{};

    constexpr UIId kPlay("play");
    constexpr UIId kQuit("quit");

    struct Fixture
    {
        UIContext ui;
        InputState input;
        InputMap map = DefaultInputMap();

        // one frame of a two-widget column under the given device; returns
        // whether each widget shows the focus highlight
        std::pair<bool, bool> Frame(DeviceKind device, float navigateY = 0.0f)
        {
            input.SetActiveDevice(device);
            ActionValue &navigate = input.Actions()[std::string(kUINavigateAction)];
            navigate.justPressed = navigateY != 0.0f && !navigate.down;
            navigate.down = navigateY != 0.0f;
            navigate.y = navigateY;

            ui.BeginFrame(kNoArea, input, map, UIPointer{});
            const UIInteraction play =
                ui.Interact(kPlay, UIRect{0, 0, 10, 10}, UIWidgetOptions{.mLinks = {kQuit, kQuit, UIId{}, UIId{}}});
            const UIInteraction quit =
                ui.Interact(kQuit, UIRect{0, 20, 10, 10}, UIWidgetOptions{.mLinks = {kPlay, kPlay, UIId{}, UIId{}}});
            ui.EndFrame();
            return {play.mShowFocus, quit.mShowFocus};
        }

        UIPrompt Prompt(DeviceKind device, std::string_view action)
        {
            input.SetActiveDevice(device);
            ui.BeginFrame(kNoArea, input, map, UIPointer{});
            const UIPrompt prompt = ui.Prompt(action);
            ui.EndFrame();
            return prompt;
        }
    };
} // namespace

TEST_CASE("The focus highlight is hidden under the mouse and shown under a gamepad or the keyboard", "[gameui]")
{
    Fixture f;
    f.ui.State().SetFocus(kPlay);

    CHECK(f.Frame(DeviceKind::Gamepad) == std::pair{true, false});
    CHECK(f.ui.FocusVisible());

    CHECK(f.Frame(DeviceKind::Mouse) == std::pair{false, false});
    CHECK_FALSE(f.ui.FocusVisible());

    CHECK(f.Frame(DeviceKind::Keyboard) == std::pair{true, false});
    CHECK(f.ui.FocusVisible());
}

TEST_CASE("Switching devices mid-menu neither moves nor loses focus", "[gameui]")
{
    Fixture f;
    f.ui.State().SetFocus(kPlay);

    f.Frame(DeviceKind::Gamepad, 1.0f);
    f.Frame(DeviceKind::Gamepad);
    REQUIRE(f.ui.State().Focus() == kQuit);

    f.Frame(DeviceKind::Mouse);
    CHECK(f.ui.State().Focus() == kQuit);

    // the press that brings the highlight back shows it, on the same frame, where focus was
    CHECK(f.Frame(DeviceKind::Gamepad, 1.0f) == std::pair{false, true});
    CHECK(f.ui.State().Focus() == kQuit);

    f.Frame(DeviceKind::Keyboard);
    CHECK(f.Frame(DeviceKind::Keyboard, 1.0f) == std::pair{false, true});
    CHECK(f.ui.State().Focus() == kPlay);
}

TEST_CASE("A prompt shows the gamepad face button under a gamepad and the key otherwise", "[gameui]")
{
    Fixture f;

    const UIPrompt pad = f.Prompt(DeviceKind::Gamepad, kUIConfirmAction);
    CHECK(pad.mLabel == "A");
    CHECK(pad.mGamepad);

    const UIPrompt keyboard = f.Prompt(DeviceKind::Keyboard, kUIConfirmAction);
    CHECK(keyboard.mLabel == "Enter");
    CHECK_FALSE(keyboard.mGamepad);

    const UIPrompt mouse = f.Prompt(DeviceKind::Mouse, kUICancelAction);
    CHECK(mouse.mLabel == "Escape");
    CHECK_FALSE(mouse.mGamepad);

    CHECK(f.Prompt(DeviceKind::Gamepad, kUICancelAction).mLabel == "B");
}

TEST_CASE("A prompt for an action not in the map is empty", "[gameui]")
{
    Fixture f;
    CHECK(f.Prompt(DeviceKind::Gamepad, "NoSuchAction").mLabel.empty());
}

TEST_CASE("HoldInput holds the game's input from the frame it is called in, until a frame does not call it",
          "[gameui]")
{
    Fixture f;
    const auto frame = [&](bool hold)
    {
        f.ui.BeginFrame(kNoArea, f.input, f.map, UIPointer{});
        if (hold)
            f.ui.HoldInput();
        f.ui.EndFrame();
    };

    frame(false);
    CHECK_FALSE(f.ui.HoldsInput());
    CHECK_FALSE(f.input.GameInputHeld());

    frame(true);
    CHECK(f.ui.HoldsInput());
    CHECK(f.input.GameInputHeld());
    frame(true);
    CHECK(f.input.GameInputHeld());

    // the editor's capture is not gameui's to set
    CHECK_FALSE(f.input.UiCapturesKeyboard());
    CHECK_FALSE(f.input.UiCapturesMouse());

    frame(false);
    CHECK_FALSE(f.ui.HoldsInput());
    CHECK_FALSE(f.input.GameInputHeld());
}
