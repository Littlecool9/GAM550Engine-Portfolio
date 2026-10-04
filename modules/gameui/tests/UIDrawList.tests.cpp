/**
 * @file UIDrawList.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIDrawList: recording, the clip stack, culling and
 *        nine-slice sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using namespace mir;

namespace
{
    // a 24x24 texture with an 8 texel border all round
    constexpr UISprite kFrame{.mTexture = UITexture{1},
                              .mTextureWidth = 24.0f,
                              .mTextureHeight = 24.0f,
                              .mSource = UIRect{0, 0, 24, 24},
                              .mSlice = UIEdges{8, 8, 8, 8}};

    std::vector<UIDrawCommand> SpriteCommands(const UIRect &rect, const UISprite &sprite, float areaSize = 1000.0f)
    {
        UIDrawList list;
        list.Reset(areaSize, areaSize);
        list.Sprite(rect, sprite);
        return {list.Commands().begin(), list.Commands().end()};
    }

    void CheckRect(const UIRect &actual, const UIRect &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x));
        CHECK(actual.y == Catch::Approx(expected.y));
        CHECK(actual.width == Catch::Approx(expected.width));
        CHECK(actual.height == Catch::Approx(expected.height));
    }
} // namespace

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

    const UITexture texture{3};
    list.Rect(UIRect{10, 10, 20, 20}, UIColor{1, 0, 0, 1}, 4.0f);
    list.Texture(UIRect{40, 40, 10, 10}, texture, kUIWhite, UIRect{0.5f, 0, 0.5f, 1});
    list.Text(5, 6, "hello", kUIWhite, 18.0f);

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 3);

    CHECK(commands[0].mKind == UIDrawCommandKind::Rect);
    CHECK(commands[0].mRect == UIRect{10, 10, 20, 20});
    CHECK(commands[0].mRounding == 4.0f);
    CHECK(commands[0].mClip == UIRect{0, 0, 100, 100});

    CHECK(commands[1].mKind == UIDrawCommandKind::Texture);
    CHECK(commands[1].mTexture == texture);
    CHECK(commands[1].mUV == UIRect{0.5f, 0, 0.5f, 1});

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
    list.Texture(UIRect{-50, 0, 10, 10}, UITexture{});
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

TEST_CASE("A sprite with no slice stretches its source over the rect as one texture", "[gameui]")
{
    // the second of four 16x16 cells in a 64x32 atlas
    const UISprite cell{.mTexture = UITexture{2},
                        .mTextureWidth = 64.0f,
                        .mTextureHeight = 32.0f,
                        .mSource = UIRect{16, 0, 16, 16}};

    UIDrawList list;
    list.Reset(500, 500);
    list.Sprite(UIRect{10, 20, 100, 50}, cell, UIColor{1, 0, 0, 1});

    REQUIRE(list.Commands().size() == 1);
    const UIDrawCommand &command = list.Commands()[0];
    CHECK(command.mKind == UIDrawCommandKind::Texture);
    CHECK(command.mTexture == UITexture{2});
    CHECK(command.mRect == UIRect{10, 20, 100, 50});
    CHECK(command.mColor == UIColor{1, 0, 0, 1});
    CheckRect(command.mUV, UIRect{0.25f, 0.0f, 0.25f, 0.5f});
}

TEST_CASE("Nine-slice keeps the corners at their texel size and stretches the edges and middle", "[gameui]")
{
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{10, 20, 200, 60}, kFrame);
    REQUIRE(commands.size() == 9);

    // row by row, left to right
    CheckRect(commands[0].mRect, UIRect{10, 20, 8, 8});
    CheckRect(commands[1].mRect, UIRect{18, 20, 184, 8});
    CheckRect(commands[2].mRect, UIRect{202, 20, 8, 8});
    CheckRect(commands[3].mRect, UIRect{10, 28, 8, 44});
    CheckRect(commands[4].mRect, UIRect{18, 28, 184, 44});
    CheckRect(commands[5].mRect, UIRect{202, 28, 8, 44});
    CheckRect(commands[6].mRect, UIRect{10, 72, 8, 8});
    CheckRect(commands[7].mRect, UIRect{18, 72, 184, 8});
    CheckRect(commands[8].mRect, UIRect{202, 72, 8, 8});

    // each slice samples its own third of the texture
    constexpr float kThird = 1.0f / 3.0f;
    CheckRect(commands[0].mUV, UIRect{0, 0, kThird, kThird});
    CheckRect(commands[4].mUV, UIRect{kThird, kThird, kThird, kThird});
    CheckRect(commands[8].mUV, UIRect{2 * kThird, 2 * kThird, kThird, kThird});
    for (const UIDrawCommand &command : commands)
        CHECK(command.mTexture == kFrame.mTexture);
}

TEST_CASE("Nine-slice corners stay the same at any size large enough to hold them", "[gameui]")
{
    for (const float width : {16.0f, 17.0f, 48.0f, 333.0f, 1000.0f})
    {
        for (const float height : {16.0f, 30.0f, 250.0f})
        {
            const UIRect rect{5, 7, width, height};
            const std::vector<UIDrawCommand> commands = SpriteCommands(rect, kFrame, 2000.0f);

            // a middle of zero width or height has no piece to draw
            const size_t columns = width > 16.0f ? 3 : 2;
            const size_t rows = height > 16.0f ? 3 : 2;
            REQUIRE(commands.size() == columns * rows);

            CheckRect(commands.front().mRect, UIRect{rect.x, rect.y, 8, 8});
            CheckRect(commands.back().mRect, UIRect{rect.Right() - 8, rect.Bottom() - 8, 8, 8});

            // the pieces cover the rect exactly
            float area = 0.0f;
            for (const UIDrawCommand &command : commands)
                area += command.mRect.width * command.mRect.height;
            CHECK(area == Catch::Approx(width * height));
        }
    }
}

TEST_CASE("Nine-slice shrinks all four corners by one factor in a rect smaller than two of them", "[gameui]")
{
    // 12 across holds 0.75 of two 8 texel corners; 40 down would hold them whole
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 12, 40}, kFrame);

    // no middle column, so two pieces per row
    REQUIRE(commands.size() == 6);
    CheckRect(commands[0].mRect, UIRect{0, 0, 6, 6});
    CheckRect(commands[1].mRect, UIRect{6, 0, 6, 6});
    CheckRect(commands[2].mRect, UIRect{0, 6, 6, 28});
    CheckRect(commands[3].mRect, UIRect{6, 6, 6, 28});
    CheckRect(commands[4].mRect, UIRect{0, 34, 6, 6});
    CheckRect(commands[5].mRect, UIRect{6, 34, 6, 6});

    // the corners still sample the whole of their texels
    CheckRect(commands[0].mUV, UIRect{0, 0, 1.0f / 3.0f, 1.0f / 3.0f});
}

TEST_CASE("Nine-slice honours a different slice on each side", "[gameui]")
{
    UISprite sprite = kFrame;
    sprite.mSlice = UIEdges{4, 2, 12, 6};
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 100, 50}, sprite);

    REQUIRE(commands.size() == 9);
    CheckRect(commands[0].mRect, UIRect{0, 0, 4, 2});
    CheckRect(commands[2].mRect, UIRect{88, 0, 12, 2});
    CheckRect(commands[6].mRect, UIRect{0, 44, 4, 6});
    CheckRect(commands[8].mRect, UIRect{88, 44, 12, 6});
    CheckRect(commands[4].mUV, UIRect{4.0f / 24.0f, 2.0f / 24.0f, 8.0f / 24.0f, 16.0f / 24.0f});
}

TEST_CASE("A slice wider than the source is clamped to it", "[gameui]")
{
    UISprite sprite = kFrame;
    sprite.mSlice = UIEdges{30, 0, 30, 0};
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 100, 10}, sprite);

    // the left slice takes all 24 texels and the right none
    REQUIRE(commands.size() == 2);
    CheckRect(commands[0].mRect, UIRect{0, 0, 24, 10});
    CheckRect(commands[0].mUV, UIRect{0, 0, 1, 1});
    CheckRect(commands[1].mRect, UIRect{24, 0, 76, 10});
}

TEST_CASE("A null sprite or an empty rect records nothing, and pieces outside the clip are culled", "[gameui]")
{
    CHECK(SpriteCommands(UIRect{0, 0, 100, 100}, UISprite{}).empty());
    CHECK(SpriteCommands(UIRect{0, 0, 0, 100}, kFrame).empty());

    UIDrawList list;
    list.Reset(100, 100);
    list.PushClip(UIRect{0, 0, 30, 100});
    list.Sprite(UIRect{0, 0, 200, 50}, kFrame);
    list.PopClip();

    // only the left column reaches the clip
    REQUIRE(list.Commands().size() == 6);
    for (const UIDrawCommand &command : list.Commands())
        CHECK(command.mRect.x < 30.0f);
}
