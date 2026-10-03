/**
 * @file UIFocus.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for focus in gameui/UIState: stepping UINavigate per direction,
 *        link traversal, skipping hidden and disabled widgets, cycles, the
 *        pointer and UINavigate sharing one slot, UIConfirm activating the
 *        focused widget, the focus highlight hiding and coming back, a
 *        widget taking left and right for itself, and pointer-only widgets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UINavInput.h>
#include <gameui/UIState.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

using namespace mir;

namespace
{
    constexpr UIRect kArea{0, 0, 800, 600};
    constexpr UIRect kClip{0, 0, 800, 600};

    constexpr UIId kPlay("play");
    constexpr UIId kOptions("options");
    constexpr UIId kQuit("quit");

    struct Widget
    {
        UIId mId;
        UIRect mRect;
        UIWidgetOptions mOptions;
    };

    constexpr UINavLinks Links(UIId up, UIId down, UIId left = {}, UIId right = {})
    {
        return UINavLinks{up, down, left, right};
    }

    // play, options and quit in a column that wraps top to bottom
    std::vector<Widget> Menu(float y = 100.0f)
    {
        return {
            {kPlay, {100, y, 200, 40}, {.mLinks = Links(kQuit, kOptions)}},
            {kOptions, {100, y + 50, 200, 40}, {.mLinks = Links(kPlay, kQuit)}},
            {kQuit, {100, y + 100, 200, 40}, {.mLinks = Links(kOptions, kPlay)}},
        };
    }

    constexpr UIPointer kRest{700, 550, false};

    // UINavigate's axes for one frame; a step comes from how they change
    constexpr UINavInput Nav(float x, float y) { return UINavInput{.mNavigateX = x, .mNavigateY = y}; }

    constexpr UINavInput kConfirm{.mConfirm = true};

    std::vector<UIInteraction> Frame(UIState &state, const std::vector<Widget> &widgets,
                                     const UIPointer &pointer = kRest, const UINavInput &nav = {},
                                     bool focusVisible = true)
    {
        state.BeginFrame(kArea, pointer, nav, focusVisible);
        std::vector<UIInteraction> results;
        for (const Widget &widget : widgets)
            results.push_back(state.Interact(widget.mId, widget.mRect, kClip, widget.mOptions));
        state.EndFrame();
        return results;
    }

    // Declares widgets in an open frame, for FollowLink
    void Declare(UIState &state, std::initializer_list<Widget> widgets)
    {
        state.BeginFrame(kArea, kRest);
        for (const Widget &widget : widgets)
            state.Interact(widget.mId, widget.mRect, kClip, widget.mOptions);
    }

    constexpr UIRect kOffscreen{-100, -100, 10, 10};
} // namespace

TEST_CASE("UINavStepper steps nothing at rest", "[gameui]")
{
    UINavStepper stepper;

    // a stick inside its binding's deadzone reaches gameui as zero
    CHECK_FALSE(stepper.Step(Nav(0, 0)).has_value());
    CHECK_FALSE(stepper.Step(UINavInput{.mNavigatePressed = true}).has_value());
}

TEST_CASE("UINavStepper steps each way from rest", "[gameui]")
{
    CHECK(UINavStepper{}.Step(Nav(1, 0)) == UINavDirection::Right);
    CHECK(UINavStepper{}.Step(Nav(-1, 0)) == UINavDirection::Left);
    CHECK(UINavStepper{}.Step(Nav(0, 1)) == UINavDirection::Down);
    CHECK(UINavStepper{}.Step(Nav(0, -1)) == UINavDirection::Up);
    CHECK(UINavStepper{}.Step(Nav(0.6f, -0.2f)) == UINavDirection::Right);
    CHECK(UINavStepper{}.Step(Nav(-0.2f, 0.3f)) == UINavDirection::Down);
}

TEST_CASE("UINavStepper steps once however long a direction is held", "[gameui]")
{
    UINavStepper stepper;

    CHECK(stepper.Step(Nav(0, 1)) == UINavDirection::Down);
    CHECK_FALSE(stepper.Step(Nav(0, 1)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0, 0.4f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0, 1)).has_value());

    CHECK_FALSE(stepper.Step(Nav(0, 0)).has_value());
    CHECK(stepper.Step(Nav(0, 1)) == UINavDirection::Down);
}

TEST_CASE("UINavStepper steps a direction tapped while another is held", "[gameui]")
{
    UINavStepper stepper;

    CHECK(stepper.Step(Nav(1, 0)) == UINavDirection::Right);

    // two arrows held together resolve to a 45 degree diagonal
    CHECK(stepper.Step(Nav(1, 1)) == UINavDirection::Down);
    CHECK_FALSE(stepper.Step(Nav(1, 1)).has_value());
    CHECK_FALSE(stepper.Step(Nav(1, 0)).has_value());
    CHECK(stepper.Step(Nav(1, -1)) == UINavDirection::Up);
    CHECK_FALSE(stepper.Step(Nav(1, 0)).has_value());

    // and the other way round: the first key let go, the second still held
    CHECK(stepper.Step(Nav(1, 1)) == UINavDirection::Down);
    CHECK_FALSE(stepper.Step(Nav(0, 1)).has_value());
    CHECK(stepper.Step(Nav(-1, 1)) == UINavDirection::Left);
}

TEST_CASE("UINavStepper steps one direction when two come on together", "[gameui]")
{
    CHECK(UINavStepper{}.Step(Nav(0.9f, 0.5f)) == UINavDirection::Right);
    CHECK(UINavStepper{}.Step(Nav(-0.5f, -0.9f)) == UINavDirection::Up);

    UINavStepper stepper;
    CHECK(stepper.Step(Nav(1, -1)) == UINavDirection::Up);

    // the other one came on too, so it does not step later while held
    CHECK_FALSE(stepper.Step(Nav(1, 0)).has_value());
}

TEST_CASE("UINavStepper ignores a stick drifting off its axis", "[gameui]")
{
    UINavStepper stepper;

    CHECK(stepper.Step(Nav(1, 0)) == UINavDirection::Right);
    CHECK_FALSE(stepper.Step(Nav(0.9f, 0.5f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0.9f, 0.85f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0.9f, -0.6f)).has_value());
}

TEST_CASE("UINavStepper steps once as the stick rolls past a diagonal, however it wobbles there", "[gameui]")
{
    UINavStepper stepper;

    CHECK(stepper.Step(Nav(1, 0)) == UINavDirection::Right);
    CHECK(stepper.Step(Nav(0.7f, 0.72f)) == UINavDirection::Down);
    CHECK_FALSE(stepper.Step(Nav(0.72f, 0.7f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0.7f, 0.72f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0.8f, 0.6f)).has_value());
    CHECK_FALSE(stepper.Step(Nav(0.6f, 0.8f)).has_value());

    // right lets go past about 63 degrees, so rolling back comes on again
    CHECK_FALSE(stepper.Step(Nav(0, 1)).has_value());
    CHECK(stepper.Step(Nav(0.72f, 0.7f)) == UINavDirection::Right);
}

TEST_CASE("FollowLink moves to the linked widget, and stays on a null link", "[gameui]")
{
    UIState state;
    Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}}, {kOptions, kOffscreen, {}}});

    CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kOptions);
    CHECK(state.FollowLink(kPlay, UINavDirection::Up) == kPlay);
    CHECK(state.FollowLink(kPlay, UINavDirection::Right) == kPlay);
    state.EndFrame();
}

TEST_CASE("FollowLink skips a hidden or disabled widget along that widget's own link", "[gameui]")
{
    UIState state;

    SECTION("hidden")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mLinks = Links(kPlay, kQuit), .mHidden = true}},
                        {kQuit, kOffscreen, {.mLinks = Links(kOptions, {})}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kQuit);
        CHECK(state.FollowLink(kQuit, UINavDirection::Up) == kPlay);
    }

    SECTION("disabled")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mLinks = Links(kPlay, kQuit), .mDisabled = true}},
                        {kQuit, kOffscreen, {.mLinks = Links(kOptions, {})}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kQuit);
    }

    SECTION("two in a row")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mLinks = Links({}, kQuit), .mHidden = true}},
                        {kQuit, kOffscreen, {.mLinks = Links({}, UIId("back")), .mDisabled = true}},
                        {UIId("back"), kOffscreen, {}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == UIId("back"));
    }

    SECTION("a skipped widget with no link onward leaves focus where it was")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mHidden = true}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kPlay);
    }

    state.EndFrame();
}

TEST_CASE("FollowLink stays put on a link to a widget not declared this frame", "[gameui]")
{
    UIState state;
    Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, UIId("missing"))}}});

    CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kPlay);
    CHECK(state.FollowLink(UIId("missing"), UINavDirection::Down) == UIId("missing"));
    state.EndFrame();
}

TEST_CASE("FollowLink wraps around a cycle", "[gameui]")
{
    UIState state;
    state.BeginFrame(kArea, kRest);
    for (const Widget &widget : Menu())
        state.Interact(widget.mId, widget.mRect, kClip, widget.mOptions);

    CHECK(state.FollowLink(kQuit, UINavDirection::Down) == kPlay);
    CHECK(state.FollowLink(kPlay, UINavDirection::Up) == kQuit);
    state.EndFrame();
}

TEST_CASE("FollowLink ends on a cycle with nothing to land on", "[gameui]")
{
    UIState state;

    SECTION("the cycle leads back to the start")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mLinks = Links({}, kQuit), .mHidden = true}},
                        {kQuit, kOffscreen, {.mLinks = Links({}, kPlay), .mDisabled = true}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kPlay);
    }

    SECTION("the cycle never returns to the start")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kOptions)}},
                        {kOptions, kOffscreen, {.mLinks = Links({}, kQuit), .mHidden = true}},
                        {kQuit, kOffscreen, {.mLinks = Links({}, kOptions), .mHidden = true}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kPlay);
    }

    SECTION("a widget linked to itself")
    {
        Declare(state, {{kPlay, kOffscreen, {.mLinks = Links({}, kPlay)}}});

        CHECK(state.FollowLink(kPlay, UINavDirection::Down) == kPlay);
    }

    state.EndFrame();
}

TEST_CASE("The first UINavigate press with nothing focused lands on the first enabled widget", "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    menu[0].mOptions.mDisabled = true;

    Frame(state, menu);
    CHECK(state.Focus().IsNull());

    Frame(state, menu, kRest, Nav(0, 1));
    CHECK(state.Focus() == kOptions);
}

TEST_CASE("UINavigate moves focus once per press, however long it is held", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);

    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(), kRest, Nav(0, 1));
    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(), kRest, UINavInput{});
    Frame(state, Menu(), kRest, Nav(0, 0.4f));
    CHECK(state.Focus() == kQuit);

    Frame(state, Menu(), kRest, UINavInput{});
    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("UINavigate moves focus the way of a direction pressed while another is held", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);

    // the menu has no right links, so holding right leaves focus on play
    Frame(state, Menu(), kRest, Nav(1, 0));
    CHECK(state.Focus() == kPlay);

    Frame(state, Menu(), kRest, Nav(1, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(), kRest, Nav(1, 0));
    Frame(state, Menu(), kRest, Nav(1, 1));
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("A widget reports focus from the frame after focus reaches it", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);

    const std::vector<UIInteraction> press = Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(press[0].mFocused);
    CHECK_FALSE(press[1].mFocused);

    const std::vector<UIInteraction> after = Frame(state, Menu());
    CHECK_FALSE(after[0].mFocused);
    CHECK(after[1].mFocused);
}

TEST_CASE("Moving the pointer onto a widget focuses it", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);
    Frame(state, Menu());

    Frame(state, Menu(), UIPointer{150, 220, false});
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("A pointer resting over a widget does not take focus back from UINavigate", "[gameui]")
{
    UIState state;
    Frame(state, Menu(), UIPointer{150, 120, false});
    Frame(state, Menu(), UIPointer{151, 120, false});
    CHECK(state.Focus() == kPlay);

    Frame(state, Menu(), UIPointer{151, 120, false}, Nav(0, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(), UIPointer{151, 120, false});
    Frame(state, Menu(), UIPointer{151, 120, false});
    CHECK(state.Focus() == kOptions);

    // the pointer moving again, even within the same widget, is the latest device
    Frame(state, Menu(), UIPointer{152, 121, false});
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("Moving the pointer off every widget leaves focus where it was", "[gameui]")
{
    UIState state;
    Frame(state, Menu(), UIPointer{150, 120, false});
    Frame(state, Menu(), UIPointer{151, 120, false});
    CHECK(state.Focus() == kPlay);

    Frame(state, Menu(), UIPointer{600, 500, false});
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("The pointer does not focus a disabled widget, which still covers the one under it", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);
    std::vector<Widget> widgets = {
        {kOptions, {100, 100, 200, 40}, {}},
        {kQuit, {100, 100, 200, 40}, {.mDisabled = true}},
    };

    Frame(state, widgets, UIPointer{150, 120, false});
    Frame(state, widgets, UIPointer{151, 120, false});
    CHECK(state.Hot() == kQuit);
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("A hidden widget is never hot", "[gameui]")
{
    UIState state;
    std::vector<Widget> widgets = {
        {kOptions, {100, 100, 200, 40}, {}},
        {kQuit, {100, 100, 200, 40}, {.mHidden = true}},
    };

    Frame(state, widgets, UIPointer{150, 120, false});
    Frame(state, widgets, UIPointer{151, 120, false});
    CHECK(state.Hot() == kOptions);
    CHECK(state.Focus() == kOptions);
}

TEST_CASE("Focus survives its widget moving between frames", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);

    Frame(state, Menu(100), kRest, Nav(0, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(300));
    CHECK(state.Focus() == kOptions);
    CHECK(Frame(state, Menu(20))[1].mFocused);

    Frame(state, Menu(450), kRest, Nav(0, 1));
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("Focus on a widget skipped for a frame is kept for when it comes back", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);

    Frame(state, {Menu()[0], Menu()[2]});
    CHECK(state.Focus() == kOptions);

    CHECK(Frame(state, Menu())[1].mFocused);
}

TEST_CASE("UIConfirm clicks the focused widget and no other", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);

    const std::vector<UIInteraction> confirm = Frame(state, Menu(), kRest, kConfirm);
    CHECK_FALSE(confirm[0].mClicked);
    CHECK(confirm[1].mClicked);
    CHECK_FALSE(confirm[2].mClicked);

    const std::vector<UIInteraction> after = Frame(state, Menu());
    CHECK_FALSE(after[1].mClicked);
}

TEST_CASE("UIConfirm does not click a focused widget that is disabled", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);
    std::vector<Widget> menu = Menu();
    menu[1].mOptions.mDisabled = true;

    CHECK_FALSE(Frame(state, menu, kRest, kConfirm)[1].mClicked);
}

TEST_CASE("UIConfirm with nothing focused clicks nothing", "[gameui]")
{
    UIState state;

    for (const UIInteraction &interaction : Frame(state, Menu(), kRest, kConfirm))
        CHECK_FALSE(interaction.mClicked);
}

// UIConfirm is bound to the primary button too and arrives a frame behind the
// pointer, so the pointer's click must not activate focus a second time
TEST_CASE("A pointer click reaches the focused widget once, not again through UIConfirm", "[gameui]")
{
    UIState state;
    state.SetFocus(kQuit);
    Frame(state, Menu(), UIPointer{600, 500, false});

    SECTION("held for a frame")
    {
        const std::vector<UIInteraction> press = Frame(state, Menu(), UIPointer{600, 500, true});
        const std::vector<UIInteraction> confirm = Frame(state, Menu(), UIPointer{600, 500, true}, kConfirm);
        const std::vector<UIInteraction> release = Frame(state, Menu(), UIPointer{600, 500, false});
        CHECK_FALSE(press[2].mClicked);
        CHECK_FALSE(confirm[2].mClicked);
        CHECK_FALSE(release[2].mClicked);
    }

    SECTION("released before UIConfirm arrives")
    {
        Frame(state, Menu(), UIPointer{600, 500, true});
        CHECK_FALSE(Frame(state, Menu(), UIPointer{600, 500, false}, kConfirm)[2].mClicked);
    }

    SECTION("on the focused widget itself")
    {
        Frame(state, Menu(), UIPointer{150, 220, false});
        Frame(state, Menu(), UIPointer{150, 220, true});
        CHECK_FALSE(Frame(state, Menu(), UIPointer{150, 220, true}, kConfirm)[2].mClicked);
        CHECK(Frame(state, Menu(), UIPointer{150, 220, false})[2].mClicked);
    }
}

TEST_CASE("Pressing the pointer on a widget focuses it", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);
    Frame(state, Menu(), UIPointer{150, 120, false});
    Frame(state, Menu(), UIPointer{150, 120, false}, Nav(0, 1));
    REQUIRE(state.Focus() == kOptions);

    Frame(state, Menu(), UIPointer{150, 120, true});
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("A hidden focus highlight keeps focus, and shows it again where it was", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);

    const std::vector<UIInteraction> hidden = Frame(state, Menu(), kRest, {}, false);
    CHECK_FALSE(state.FocusVisible());
    CHECK(hidden[1].mFocused);
    CHECK_FALSE(hidden[1].mShowFocus);
    CHECK(state.Focus() == kOptions);

    const std::vector<UIInteraction> shown = Frame(state, Menu());
    CHECK(state.FocusVisible());
    CHECK(shown[1].mShowFocus);
    CHECK_FALSE(shown[0].mShowFocus);
    CHECK_FALSE(shown[2].mShowFocus);
    CHECK(state.Focus() == kOptions);
}

TEST_CASE("The UINavigate press that brings the highlight back does not move focus", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);
    Frame(state, Menu(), kRest, {}, false);

    CHECK(Frame(state, Menu(), kRest, Nav(0, 1))[1].mShowFocus);
    CHECK(state.Focus() == kOptions);

    // held, it does not step either; let go and pressed again, it does
    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kOptions);

    Frame(state, Menu(), kRest, UINavInput{});
    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("The UIConfirm press that brings the highlight back clicks nothing", "[gameui]")
{
    UIState state;
    state.SetFocus(kOptions);
    Frame(state, Menu(), kRest, {}, false);

    for (const UIInteraction &interaction : Frame(state, Menu(), kRest, kConfirm))
        CHECK_FALSE(interaction.mClicked);

    Frame(state, Menu());
    CHECK(Frame(state, Menu(), kRest, kConfirm)[1].mClicked);
}

TEST_CASE("The UINavigate press that brings the highlight back still lands on a widget when none is focused",
          "[gameui]")
{
    UIState state;
    Frame(state, Menu(), kRest, {}, false);

    Frame(state, Menu(), kRest, Nav(0, 1));
    CHECK(state.Focus() == kPlay);
}

TEST_CASE("The pointer moves focus while the highlight is hidden, and it comes back there", "[gameui]")
{
    UIState state;
    state.SetFocus(kPlay);
    Frame(state, Menu(), kRest, {}, false);

    Frame(state, Menu(), UIPointer{150, 220, false}, {}, false);
    CHECK(state.Focus() == kQuit);

    Frame(state, Menu(), UIPointer{600, 500, false}, {}, false);
    CHECK(state.Focus() == kQuit);

    const std::vector<UIInteraction> shown = Frame(state, Menu(), UIPointer{600, 500, false}, Nav(0, 1));
    CHECK(shown[2].mShowFocus);
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("A focused widget with mAdjustsX takes left and right as mAdjust, and focus stays", "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    menu[1].mOptions.mLinks = Links(kPlay, kQuit, kPlay, kQuit);
    menu[1].mOptions.mAdjustsX = true;
    state.SetFocus(kOptions);

    const std::vector<UIInteraction> right = Frame(state, menu, kRest, Nav(1, 0));
    CHECK(right[1].mAdjust == 1);
    CHECK(right[0].mAdjust == 0);
    CHECK(state.Focus() == kOptions);

    Frame(state, menu);
    CHECK(Frame(state, menu, kRest, Nav(-1, 0))[1].mAdjust == -1);
    CHECK(state.Focus() == kOptions);

    // held, it adjusts once
    CHECK(Frame(state, menu, kRest, Nav(-1, 0))[1].mAdjust == 0);
}

TEST_CASE("A widget with mAdjustsX still lets up and down follow its links", "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    menu[1].mOptions.mAdjustsX = true;
    state.SetFocus(kOptions);

    CHECK(Frame(state, menu, kRest, Nav(0, 1))[1].mAdjust == 0);
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("mAdjust goes only to the focused widget, and not on the press that brings the highlight back",
          "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    for (Widget &widget : menu)
        widget.mOptions.mAdjustsX = true;
    state.SetFocus(kQuit);

    Frame(state, menu, kRest, {}, false);
    for (const UIInteraction &interaction : Frame(state, menu, kRest, Nav(1, 0)))
        CHECK(interaction.mAdjust == 0);

    Frame(state, menu);
    const std::vector<UIInteraction> right = Frame(state, menu, kRest, Nav(1, 0));
    CHECK(right[0].mAdjust == 0);
    CHECK(right[1].mAdjust == 0);
    CHECK(right[2].mAdjust == 1);
}

TEST_CASE("A widget without mAdjustsX follows its left and right links as before", "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    menu[1].mOptions.mLinks = Links(kPlay, kQuit, kPlay, kQuit);
    state.SetFocus(kOptions);

    CHECK(Frame(state, menu, kRest, Nav(1, 0))[1].mAdjust == 0);
    CHECK(state.Focus() == kQuit);
}

TEST_CASE("A pointer-only widget is clicked by the pointer and never focused", "[gameui]")
{
    UIState state;
    constexpr UIId kHud("hud");
    const std::vector<Widget> widgets{{kHud, {100, 100, 200, 40}, {.mPointerOnly = true}}};
    constexpr UIPointer kOver{150, 120, false};
    constexpr UIPointer kPressOver{150, 120, true};

    // no first UINavigate press lands on it, and UIConfirm clicks nothing
    Frame(state, widgets, kRest, Nav(0, 1));
    Frame(state, widgets);
    CHECK(state.Focus().IsNull());
    CHECK_FALSE(Frame(state, widgets, kRest, kConfirm)[0].mClicked);

    // the pointer hovers and clicks it, and leaves focus alone
    Frame(state, widgets, kOver);
    CHECK(Frame(state, widgets, kOver)[0].mHovered);
    Frame(state, widgets, kPressOver);
    CHECK(Frame(state, widgets, kOver)[0].mClicked);
    CHECK(state.Focus().IsNull());

    // focus set on it by hand is not shown or confirmed
    state.SetFocus(kHud);
    const UIInteraction named = Frame(state, widgets, kOver, kConfirm)[0];
    CHECK_FALSE(named.mFocused);
    CHECK_FALSE(named.mShowFocus);
    CHECK_FALSE(named.mClicked);
}

TEST_CASE("Links pass over a pointer-only widget, as over a disabled one", "[gameui]")
{
    UIState state;
    std::vector<Widget> menu = Menu();
    menu[1].mOptions.mPointerOnly = true;
    state.SetFocus(kPlay);

    Frame(state, menu, kRest, Nav(0, 1));
    CHECK(state.Focus() == kQuit);
}
