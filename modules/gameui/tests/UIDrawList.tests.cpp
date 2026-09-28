/**
 * @file UIDrawList.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIDrawList: recording, the clip stack and culling.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("Intersect of disjoint rects is empty, not negative", "[gameui]")
{
    const UIRect overlap = Intersect(UIRect{0, 0, 10, 10}, UIRect{20, 20, 5, 5});

    CHECK(overlap.IsEmpty());
    CHECK(overlap.width == 0.0f);
    CHECK(overlap.height == 0.0f);
}

TEST_CASE("Reset sets the root clip to the area and empties the list", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 50);
    list.Rect(UIRect{0, 0, 10, 10}, kUIWhite);
    list.Reset(200, 80);

    CHECK(list.Commands().empty());
    CHECK(list.ClipDepth() == 0);
    CHECK(list.CurrentClip() == UIRect{0, 0, 200, 80});
}

TEST_CASE("Commands record in order with their kind and the current clip", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    const TextureHandle texture{.mIndex = 3, .mGeneration = 1};
    list.Rect(UIRect{10, 10, 20, 20}, UIColor{1, 0, 0, 1}, 4.0f);
    list.Texture(UIRect{40, 40, 10, 10}, texture);
    list.Text(5, 6, "hello", kUIWhite, 18.0f);

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 3);

    CHECK(commands[0].mKind == UIDrawCommandKind::Rect);
    CHECK(commands[0].mRect == UIRect{10, 10, 20, 20});
    CHECK(commands[0].mRounding == 4.0f);
    CHECK(commands[0].mClip == UIRect{0, 0, 100, 100});

    CHECK(commands[1].mKind == UIDrawCommandKind::Texture);
    CHECK(commands[1].mTexture == texture);

    CHECK(commands[2].mKind == UIDrawCommandKind::Text);
    CHECK(commands[2].mRect.x == 5.0f);
    CHECK(commands[2].mRect.y == 6.0f);
    CHECK(commands[2].mFontSize == 18.0f);
    CHECK(list.TextOf(commands[2]) == "hello");
}

TEST_CASE("Text keeps each string separate in the shared buffer", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);
    list.Text(0, 0, "first", kUIWhite);
    list.Text(0, 20, "second", kUIWhite);

    REQUIRE(list.Commands().size() == 2);
    CHECK(list.TextOf(list.Commands()[0]) == "first");
    CHECK(list.TextOf(list.Commands()[1]) == "second");
}

TEST_CASE("PushClip intersects with the current clip and PopClip restores it", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.PushClip(UIRect{50, 50, 100, 100});
    CHECK(list.ClipDepth() == 1);
    CHECK(list.CurrentClip() == UIRect{50, 50, 50, 50});

    list.PushClip(UIRect{0, 0, 60, 60});
    CHECK(list.CurrentClip() == UIRect{50, 50, 10, 10});

    list.Rect(UIRect{0, 0, 100, 100}, kUIWhite);
    REQUIRE(list.Commands().size() == 1);
    CHECK(list.Commands()[0].mClip == UIRect{50, 50, 10, 10});

    list.PopClip();
    list.PopClip();
    CHECK(list.ClipDepth() == 0);
    CHECK(list.CurrentClip() == UIRect{0, 0, 100, 100});
}

TEST_CASE("Draws entirely outside the clip are not recorded", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.Rect(UIRect{200, 200, 10, 10}, kUIWhite);
    list.Texture(UIRect{-50, 0, 10, 10}, kNullTexture);
    CHECK(list.Commands().empty());

    // text is not measured, so only an empty clip culls it
    list.Text(500, 500, "offscreen", kUIWhite);
    CHECK(list.Commands().size() == 1);

    list.PushClip(UIRect{200, 200, 10, 10});
    list.Text(0, 0, "clipped away", kUIWhite);
    list.Rect(UIRect{0, 0, 100, 100}, kUIWhite);
    list.PopClip();
    CHECK(list.Commands().size() == 1);
}

TEST_CASE("TextOf a non-text command is empty", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);
    list.Rect(UIRect{0, 0, 10, 10}, kUIWhite);

    REQUIRE(list.Commands().size() == 1);
    CHECK(list.TextOf(list.Commands()[0]).empty());
}
