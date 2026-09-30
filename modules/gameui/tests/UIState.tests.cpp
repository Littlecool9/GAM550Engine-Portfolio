/**
 * @file UIState.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIState: hot and active, the press latch, and hit
 *        testing against the clip stack and the area's origin.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>
#include <gameui/UIState.h>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace mir;

namespace
{
    constexpr UIRect kArea{0, 0, 800, 600};
    constexpr UIRect kButton{100, 100, 200, 50};
    constexpr UIId kButtonId("button");

    constexpr UIPointer Up(float x, float y) { return UIPointer{x, y, false}; }
    constexpr UIPointer Down(float x, float y) { return UIPointer{x, y, true}; }

    // A frame with one widget, clipped only by the area
    UIInteraction Frame(UIState &state, const UIPointer &pointer, const UIRect &rect = kButton,
                        const UIRect &area = kArea)
    {
        state.BeginFrame(area, pointer);
        const UIInteraction result = state.Interact(kButtonId, rect, UIRect{0, 0, area.width, area.height});
        state.EndFrame();
        return result;
    }
} // namespace

TEST_CASE("Contains takes the left and top edges but not the right and bottom", "[gameui]")
{
    constexpr UIRect rect{10, 10, 20, 20};

    CHECK(Contains(rect, 10, 10));
    CHECK(Contains(rect, 29.5f, 29.5f));
    CHECK_FALSE(Contains(rect, 30, 20));
    CHECK_FALSE(Contains(rect, 20, 30));
    CHECK_FALSE(Contains(UIRect{10, 10, 0, 0}, 10, 10));
}

TEST_CASE("A widget under the pointer is hot from the next frame until the pointer leaves", "[gameui]")
{
    UIState state;

    CHECK_FALSE(Frame(state, Up(150, 120)).mHovered);
    CHECK(state.Hot() == kButtonId);

    CHECK(Frame(state, Up(150, 120)).mHovered);

    CHECK_FALSE(Frame(state, Up(10, 10)).mHovered);
    CHECK(state.Hot().IsNull());
}

TEST_CASE("Press and release over a widget clicks it once", "[gameui]")
{
    UIState state;
    Frame(state, Up(150, 120));

    const UIInteraction press = Frame(state, Down(150, 120));
    CHECK_FALSE(press.mClicked);
    CHECK(state.Active() == kButtonId);

    const UIInteraction hold = Frame(state, Down(150, 120));
    CHECK(hold.mHeld);
    CHECK(hold.mHovered);
    CHECK_FALSE(hold.mClicked);

    const UIInteraction release = Frame(state, Up(150, 120));
    CHECK(release.mClicked);
    CHECK_FALSE(release.mHeld);
    CHECK(state.Active().IsNull());

    CHECK_FALSE(Frame(state, Up(150, 120)).mClicked);
}

TEST_CASE("Press, drag away, release does not click", "[gameui]")
{
    UIState state;
    Frame(state, Up(150, 120));
    Frame(state, Down(150, 120));

    // the latch holds with the pointer off the widget
    const UIInteraction dragged = Frame(state, Down(500, 400));
    CHECK(dragged.mHeld);
    CHECK_FALSE(dragged.mHovered);
    CHECK(state.Active() == kButtonId);

    const UIInteraction release = Frame(state, Up(500, 400));
    CHECK_FALSE(release.mClicked);
    CHECK(state.Active().IsNull());
}

TEST_CASE("Dragging back onto the pressed widget before release still clicks", "[gameui]")
{
    UIState state;
    Frame(state, Up(150, 120));
    Frame(state, Down(150, 120));
    Frame(state, Down(500, 400));
    Frame(state, Down(150, 120));

    CHECK(Frame(state, Up(150, 120)).mClicked);
}

TEST_CASE("A press that begins off every widget clicks nothing it is released over", "[gameui]")
{
    UIState state;
    Frame(state, Up(500, 400));
    Frame(state, Down(500, 400));

    // held from elsewhere, so the widget does not even become hot
    CHECK_FALSE(Frame(state, Down(150, 120)).mHovered);
    CHECK(state.Hot().IsNull());
    CHECK(state.Active().IsNull());

    CHECK_FALSE(Frame(state, Up(150, 120)).mClicked);
}

TEST_CASE("While one widget is held no other widget becomes hot", "[gameui]")
{
    constexpr UIId other("other");
    constexpr UIRect otherRect{400, 100, 200, 50};

    UIState state;
    auto frame = [&](const UIPointer &pointer)
    {
        state.BeginFrame(kArea, pointer);
        state.Interact(kButtonId, kButton, kArea);
        const UIInteraction result = state.Interact(other, otherRect, kArea);
        state.EndFrame();
        return result;
    };

    frame(Up(150, 120));
    frame(Down(150, 120));
    frame(Down(450, 120));
    CHECK_FALSE(frame(Down(450, 120)).mHovered);
    CHECK(state.Hot().IsNull());

    CHECK_FALSE(frame(Up(450, 120)).mClicked);

    // released, so it is reachable again
    frame(Up(450, 120));
    CHECK(state.Hot() == other);
}

TEST_CASE("The id, not the rect, carries a press across a widget that moves", "[gameui]")
{
    UIState state;
    Frame(state, Up(150, 120));
    Frame(state, Down(150, 120));

    // drawn somewhere else while held, with the pointer following it
    constexpr UIRect moved{300, 300, 200, 50};
    CHECK(Frame(state, Down(350, 320), moved).mHeld);
    CHECK(state.Active() == kButtonId);

    CHECK(Frame(state, Up(350, 320), moved).mClicked);
}

TEST_CASE("Where widgets overlap the one drawn last takes the pointer", "[gameui]")
{
    constexpr UIId below("below");
    constexpr UIId above("above");

    UIState state;
    auto frame = [&](const UIPointer &pointer)
    {
        state.BeginFrame(kArea, pointer);
        const UIInteraction first = state.Interact(below, UIRect{100, 100, 200, 200}, kArea);
        const UIInteraction second = state.Interact(above, UIRect{150, 150, 200, 200}, kArea);
        state.EndFrame();
        return std::pair{first, second};
    };

    frame(Up(200, 200));
    CHECK(state.Hot() == above);

    frame(Down(200, 200));
    CHECK(state.Active() == above);

    const auto [first, second] = frame(Up(200, 200));
    CHECK_FALSE(first.mClicked);
    CHECK(second.mClicked);
}

TEST_CASE("A widget clipped out by the clip stack cannot be hovered or pressed", "[gameui]")
{
    UIDrawList list;
    UIState state;

    // the clip cuts the button at x = 150
    auto frame = [&](const UIPointer &pointer)
    {
        list.Reset(kArea.width, kArea.height);
        state.BeginFrame(kArea, pointer);
        list.PushClip(UIRect{0, 0, 150, 600});
        const UIInteraction result = state.Interact(kButtonId, kButton, list.CurrentClip());
        list.PopClip();
        state.EndFrame();
        return result;
    };

    SECTION("the clipped part is not hoverable")
    {
        frame(Up(200, 120));
        CHECK_FALSE(frame(Up(200, 120)).mHovered);
        CHECK(state.Hot().IsNull());

        frame(Down(200, 120));
        CHECK_FALSE(frame(Up(200, 120)).mClicked);
    }

    SECTION("the visible part still is")
    {
        frame(Up(120, 120));
        CHECK(frame(Up(120, 120)).mHovered);
    }

    SECTION("a widget clipped out entirely is never hot")
    {
        auto hidden = [&](const UIPointer &pointer)
        {
            list.Reset(kArea.width, kArea.height);
            state.BeginFrame(kArea, pointer);
            list.PushClip(UIRect{600, 500, 100, 100});
            const UIInteraction result = state.Interact(kButtonId, kButton, list.CurrentClip());
            list.PopClip();
            state.EndFrame();
            return result;
        };

        hidden(Up(150, 120));
        hidden(Down(150, 120));
        CHECK(state.Hot().IsNull());
        CHECK(state.Active().IsNull());
        CHECK_FALSE(hidden(Up(150, 120)).mClicked);
    }
}

TEST_CASE("Hit testing is relative to the area's origin, not the window's", "[gameui]")
{
    // the scene viewport, docked away from the window's top-left
    constexpr UIRect viewport{300, 120, 800, 600};
    constexpr UIRect panel{16, 16, 220, 40};

    UIState state;

    SECTION("the pointer over the widget where it is drawn hits it")
    {
        Frame(state, Up(320, 140), panel, viewport);
        CHECK(Frame(state, Up(320, 140), panel, viewport).mHovered);
        CHECK(state.PointerX() == 20.0f);
        CHECK(state.PointerY() == 20.0f);

        Frame(state, Down(320, 140), panel, viewport);
        CHECK(Frame(state, Up(320, 140), panel, viewport).mClicked);
    }

    SECTION("the pointer where the widget would be at 0,0 misses it")
    {
        Frame(state, Up(20, 20), panel, viewport);
        CHECK_FALSE(Frame(state, Up(20, 20), panel, viewport).mHovered);
        CHECK(state.Hot().IsNull());
    }
}

TEST_CASE("A pointer outside the area misses a widget that overhangs it", "[gameui]")
{
    constexpr UIRect viewport{300, 120, 400, 300};
    constexpr UIRect overhanging{350, 10, 200, 40};
    constexpr UIRect unclipped{0, 0, 10000, 10000};

    UIState state;
    auto frame = [&](const UIPointer &pointer)
    {
        state.BeginFrame(viewport, pointer);
        const UIInteraction result = state.Interact(kButtonId, overhanging, unclipped);
        state.EndFrame();
        return result;
    };

    // local x 420 is inside the rect but past the area's right edge at 400
    frame(Up(720, 150));
    CHECK(state.Hot().IsNull());

    // local x 380 is inside both
    frame(Up(680, 150));
    CHECK(state.Hot() == kButtonId);
}

TEST_CASE("A held widget that stops being drawn lets go of the pointer", "[gameui]")
{
    UIState state;
    Frame(state, Up(150, 120));
    Frame(state, Down(150, 120));
    REQUIRE(state.Active() == kButtonId);

    state.BeginFrame(kArea, Down(150, 120));
    state.EndFrame();
    CHECK(state.Active().IsNull());

    // back, but the press that latched it is gone
    CHECK_FALSE(Frame(state, Down(150, 120)).mHeld);
    CHECK_FALSE(Frame(state, Up(150, 120)).mClicked);
}

TEST_CASE("Widgets given one id in a frame are counted and act as one", "[gameui]")
{
    constexpr UIRect left{100, 100, 100, 50};
    constexpr UIRect right{400, 100, 100, 50};

    UIState state;
    auto frame = [&](const UIPointer &pointer)
    {
        state.BeginFrame(kArea, pointer);
        const UIInteraction first = state.Interact(kButtonId, left, kArea);
        const UIInteraction second = state.Interact(kButtonId, right, kArea);
        state.EndFrame();
        return std::pair{first, second};
    };

    frame(Up(150, 120));
    CHECK(state.Collisions() == 1);

    // pressing one holds both
    frame(Down(150, 120));
    const auto [first, second] = frame(Down(150, 120));
    CHECK(first.mHeld);
    CHECK(second.mHeld);

    // a new frame counts afresh
    state.BeginFrame(kArea, Down(150, 120));
    state.Interact(kButtonId, left, kArea);
    CHECK(state.Collisions() == 0);
    state.EndFrame();
}
