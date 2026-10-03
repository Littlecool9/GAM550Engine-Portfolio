/**
 * @file UINavInput.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UINavInput: the UI actions are read from InputState
 *        by name, whichever device set them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UINavInput.h>

#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace mir;

namespace
{
    ActionValue &Set(InputState &input, std::string_view action)
    {
        return input.Actions()[std::string(action)];
    }
} // namespace

TEST_CASE("ReadNavInput is all zero when no UI action is in the state", "[gameui]")
{
    const UINavInput nav = ReadNavInput(InputState{});

    CHECK(nav.mNavigateX == 0.0f);
    CHECK(nav.mNavigateY == 0.0f);
    CHECK_FALSE(nav.mNavigatePressed);
    CHECK_FALSE(nav.mConfirm);
    CHECK_FALSE(nav.mCancel);
    CHECK_FALSE(nav.mTabLeft);
    CHECK_FALSE(nav.mTabRight);
}

TEST_CASE("ReadNavInput carries UINavigate's axes and its press", "[gameui]")
{
    InputState input;
    Set(input, kUINavigateAction) = ActionValue{.down = true, .justPressed = true, .x = -0.5f, .y = 1.0f};

    const UINavInput nav = ReadNavInput(input);
    CHECK(nav.mNavigateX == -0.5f);
    CHECK(nav.mNavigateY == 1.0f);
    CHECK(nav.mNavigatePressed);
}

TEST_CASE("ReadNavInput reports a button only on the frame it was pressed", "[gameui]")
{
    InputState input;
    Set(input, kUIConfirmAction) = ActionValue{.down = true, .justPressed = true};
    Set(input, kUICancelAction) = ActionValue{.down = true, .justPressed = false};
    Set(input, kUITabLeftAction) = ActionValue{.down = true, .justPressed = true};
    Set(input, kUITabRightAction) = ActionValue{.justReleased = true};
    Set(input, kUIPauseAction) = ActionValue{.down = true, .justPressed = true};

    const UINavInput nav = ReadNavInput(input);
    CHECK(nav.mConfirm);
    CHECK_FALSE(nav.mCancel);
    CHECK(nav.mTabLeft);
    CHECK_FALSE(nav.mTabRight);
    CHECK(nav.mPause);
}
