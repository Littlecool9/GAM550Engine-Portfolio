/**
 * @file UILayout.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UILayout: inset, anchor, rows and columns, the three
 *        nested, and the demo screen's layout reflowing across aspect ratios
 *        and safe areas.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDemoScreen.h>
#include <gameui/UILayout.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace mir;

namespace
{
    void CheckRect(const UIRect &actual, const UIRect &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x));
        CHECK(actual.y == Catch::Approx(expected.y));
        CHECK(actual.width == Catch::Approx(expected.width));
        CHECK(actual.height == Catch::Approx(expected.height));
    }

    // inside, edges included, with a little float slack
    bool Within(const UIRect &inner, const UIRect &outer)
    {
        constexpr float kSlack = 0.001f;
        return inner.x >= outer.x - kSlack && inner.y >= outer.y - kSlack &&
               inner.Right() <= outer.Right() + kSlack && inner.Bottom() <= outer.Bottom() + kSlack;
    }

    bool Overlap(const UIRect &a, const UIRect &b) { return !Intersect(a, b).IsEmpty(); }
} // namespace

TEST_CASE("Inset takes each edge in, and collapses rather than going negative", "[gameui][layout]")
{
    CheckRect(Inset(UIRect{10, 20, 100, 50}, UIEdges{1, 2, 3, 4}), UIRect{11, 22, 96, 44});
    CheckRect(Inset(UIRect{10, 20, 100, 50}, 5.0f), UIRect{15, 25, 90, 40});

    const UIRect collapsed = Inset(UIRect{0, 0, 10, 10}, UIEdges{8, 0, 8, 0});
    CHECK(collapsed.width == 0.0f);
    CHECK(collapsed.height == 10.0f);
    CHECK(collapsed.IsEmpty());

    static_assert(Inset(UIRect{0, 0, 10, 10}, 1.0f) == UIRect{1, 1, 8, 8});
}

TEST_CASE("Anchor puts a rect on the same point of its container", "[gameui][layout]")
{
    constexpr UIRect kContainer{100, 50, 400, 200};

    CheckRect(Anchor(kContainer, kUITopLeft, 40, 20), UIRect{100, 50, 40, 20});
    CheckRect(Anchor(kContainer, kUITop, 40, 20), UIRect{280, 50, 40, 20});
    CheckRect(Anchor(kContainer, kUITopRight, 40, 20), UIRect{460, 50, 40, 20});
    CheckRect(Anchor(kContainer, kUILeft, 40, 20), UIRect{100, 140, 40, 20});
    CheckRect(Anchor(kContainer, kUICenter, 40, 20), UIRect{280, 140, 40, 20});
    CheckRect(Anchor(kContainer, kUIRight, 40, 20), UIRect{460, 140, 40, 20});
    CheckRect(Anchor(kContainer, kUIBottomLeft, 40, 20), UIRect{100, 230, 40, 20});
    CheckRect(Anchor(kContainer, kUIBottom, 40, 20), UIRect{280, 230, 40, 20});
    CheckRect(Anchor(kContainer, kUIBottomRight, 40, 20), UIRect{460, 230, 40, 20});

    // larger than its container: overhangs evenly about the anchor
    CheckRect(Anchor(kContainer, kUICenter, 500, 200), UIRect{50, 50, 500, 200});

    // a margin is an inset of the container first
    CheckRect(Anchor(Inset(kContainer, 10.0f), kUIBottomRight, 40, 20), UIRect{450, 220, 40, 20});
}

TEST_CASE("A column cuts rects down from the top, a gap apart, each the full width", "[gameui][layout]")
{
    UIStack column = UIStack::Column(UIRect{10, 20, 100, 200}, 5.0f);

    CheckRect(column.Next(30), UIRect{10, 20, 100, 30});
    CheckRect(column.Next(40), UIRect{10, 55, 100, 40});
    CHECK(column.Remaining() == Catch::Approx(120));
    CheckRect(column.Rest(), UIRect{10, 100, 100, 120});
    CHECK(column.Remaining() == 0.0f);
}

TEST_CASE("A row cuts from both ends, and Rest takes what lies between", "[gameui][layout]")
{
    UIStack row = UIStack::Row(UIRect{0, 10, 300, 24}, 10.0f);

    CheckRect(row.Last(80), UIRect{220, 10, 80, 24});
    CheckRect(row.Next(50), UIRect{0, 10, 50, 24});
    CheckRect(row.Last(20), UIRect{190, 10, 20, 24});

    // a gap clear of both neighbours
    CheckRect(row.Rest(), UIRect{60, 10, 120, 24});
}

TEST_CASE("A stack that runs out clamps its cuts and then hands out empty rects", "[gameui][layout]")
{
    UIStack column = UIStack::Column(UIRect{0, 0, 50, 100}, 10.0f);

    CheckRect(column.Next(70), UIRect{0, 0, 50, 70});
    CheckRect(column.Next(70), UIRect{0, 80, 50, 20});
    CHECK(column.Next(10).IsEmpty());
    CHECK(column.Last(10).IsEmpty());
    CHECK(column.Rest().IsEmpty());

    // a negative size takes nothing
    UIStack row = UIStack::Row(UIRect{0, 0, 100, 10});
    CHECK(row.Next(-5).IsEmpty());
    CHECK(row.Remaining() == 100.0f);
}

TEST_CASE("Length is what a stack of those sizes needs, gaps included", "[gameui][layout]")
{
    static_assert(UIStack::Length({}, 10.0f) == 0.0f);
    static_assert(UIStack::Length({40.0f}, 10.0f) == 40.0f);
    static_assert(UIStack::Length({40.0f, 24.0f, 16.0f}, 10.0f) == 100.0f);

    // a column sized by Length holds exactly those cuts
    UIStack column = UIStack::Column(UIRect{0, 0, 10, UIStack::Length({40, 24, 16}, 10)}, 10);
    column.Next(40);
    column.Next(24);
    CHECK(column.Next(16).height == 16.0f);
    CHECK(column.Remaining() == 0.0f);
}

TEST_CASE("A row inside a column inside an anchored region", "[gameui][layout]")
{
    // a 300 by 130 region in the bottom-right of a 1000 by 600 area, 20 in from its edges
    const UIRect region = Anchor(Inset(UIRect{0, 0, 1000, 600}, 20.0f), kUIBottomRight, 300, 130);
    CheckRect(region, UIRect{680, 450, 300, 130});

    UIStack column = UIStack::Column(region, 5.0f);
    const UIRect header = column.Next(40);
    UIStack row = UIStack::Row(column.Next(40), 5.0f);
    const UIRect icon = row.Next(40);
    const UIRect value = row.Last(60);
    const UIRect name = row.Rest();
    const UIRect footer = column.Rest();

    CheckRect(header, UIRect{680, 450, 300, 40});
    CheckRect(icon, UIRect{680, 495, 40, 40});
    CheckRect(name, UIRect{725, 495, 190, 40});
    CheckRect(value, UIRect{920, 495, 60, 40});
    CheckRect(footer, UIRect{680, 540, 300, 40});
}

TEST_CASE("The demo menu reflows at 16:9, 21:9 and 4:3 inside any safe area", "[gameui][layout]")
{
    struct Screen
    {
        float mWidth;
        float mHeight;
    };
    const std::vector<Screen> screens{{1920, 1080}, {2560, 1080}, {1440, 1080},
                                      {1280, 720},  {3440, 1440}, {1024, 768}};

    for (const Screen screen : screens)
    {
        // a TV's 5% title-safe margin, none, and a lopsided one like a notch
        const std::vector<UIEdges> insetsToTry{
            UIEdges{screen.mWidth * 0.05f, screen.mHeight * 0.05f, screen.mWidth * 0.05f, screen.mHeight * 0.05f},
            UIEdges{},
            UIEdges{120, 0, 0, 30}};

        for (const UIEdges &insets : insetsToTry)
        {
            const UIRect safe = Inset(UIRect{0, 0, screen.mWidth, screen.mHeight}, insets);
            const UIDemoLayout layout = UIDemoScreen::Layout(safe);
            INFO(std::to_string(screen.mWidth) + "x" + std::to_string(screen.mHeight) + ", left inset " +
                 std::to_string(insets.mLeft));

            const std::vector<UIRect> rows{layout.mTitle,  layout.mButton, layout.mToggle,
                                           layout.mDisabled, layout.mSlider, layout.mBar, layout.mImage};
            const std::vector<UIRect> all{layout.mPanel, layout.mTitle,  layout.mButton, layout.mToggle,
                                          layout.mDisabled, layout.mSlider, layout.mVolume, layout.mBar,
                                          layout.mImage,  layout.mCaption, layout.mPrompts};

            // everything inside the safe area, and nothing empty
            for (const UIRect &rect : all)
            {
                CHECK(Within(rect, safe));
                CHECK_FALSE(rect.IsEmpty());
            }

            // the menu down the left of the safe area, centred in what the prompts leave
            CHECK(layout.mPanel.x == Catch::Approx(safe.x + 12.0f));
            const float above = layout.mPanel.y - (safe.y + 12.0f);
            const float below = layout.mPrompts.y - 12.0f - layout.mPanel.Bottom();
            CHECK(above == Catch::Approx(below));
            const float share = std::clamp((safe.width - 24.0f) * 0.3f, 320.0f, 480.0f);
            CHECK(layout.mPanel.width == Catch::Approx(share));

            // its rows top to bottom, inside it, apart
            for (size_t i = 0; i < rows.size(); ++i)
            {
                CHECK(Within(rows[i], layout.mPanel));
                if (i > 0)
                    CHECK(rows[i].y > rows[i - 1].Bottom());
            }

            // the percentage keeps its width at the right, the slider the rest
            CHECK(layout.mVolume.width == Catch::Approx(80.0f));
            CHECK(layout.mVolume.Right() == Catch::Approx(layout.mPanel.Right() - 12.0f));
            CHECK(layout.mSlider.Right() + 12.0f == Catch::Approx(layout.mVolume.x));
            CHECK(layout.mImage.width == Catch::Approx(layout.mImage.height));
            CHECK_FALSE(Overlap(layout.mImage, layout.mCaption));

            // the prompts along the bottom of the safe area, clear of the menu
            CHECK(layout.mPrompts.Bottom() == Catch::Approx(safe.Bottom() - 12.0f));
            CHECK(layout.mPrompts.width == Catch::Approx(safe.width - 24.0f));
            CHECK_FALSE(Overlap(layout.mPrompts, layout.mPanel));
        }
    }
}
