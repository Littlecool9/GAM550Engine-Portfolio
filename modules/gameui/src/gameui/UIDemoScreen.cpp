/**
 * @file UIDemoScreen.cpp
 * @author Quy Dong Ta
 * @brief A debug screen that exercises every core widget, shown by App under
 *        --ui-demo, in grey box or in the art under assets/ui/. It holds the
 *        values its widgets edit, as any screen would.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDemoScreen.h>

#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>

#include <assets/AssetId.h>
#include <core/log/Log.h>
#include <input/DefaultInputMap.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

namespace mir
{
    namespace
    {
        constexpr UIId kButton("demo_button");
        constexpr UIId kToggle("demo_toggle");
        constexpr UIId kDisabled("demo_disabled");
        constexpr UIId kSlider("demo_slider");


        constexpr float kMargin = 12.0f;
        constexpr float kPadding = 12.0f;
        constexpr float kGap = 12.0f;
        constexpr float kRow = 40.0f;
        constexpr float kSliderRow = 24.0f;
        constexpr float kBarRow = 16.0f;
        constexpr float kVolumeWidth = 80.0f;
        constexpr float kPromptHeight = 24.0f;
        constexpr float kPromptGap = 16.0f;

        constexpr float kMenuShare = 0.3f;
        constexpr float kMenuMinWidth = 320.0f;
        constexpr float kMenuMaxWidth = 480.0f;

        constexpr float kMenuHeight =
            UIStack::Length({kRow, kRow, kRow, kRow, kSliderRow, kBarRow, kRow}, kGap) + 2.0f * kPadding;


        UISprite Sliced(UISprite sprite, float slice)
        {
            sprite.mSlice = UIEdges{slice, slice, slice, slice};
            return sprite;
        }


        float TextWidth(std::string_view text) { return 8.0f * static_cast<float>(text.size()); }

        void DrawPrompt(UIContext &ui, UIStack &strip, std::string_view action, std::string_view text)
        {
            const UIPrompt prompt = ui.Prompt(action);
            if (prompt.mLabel.empty())
                return;

            const UIStyle &style = ui.Style();
            const float capWidth = std::max(kPromptHeight, TextWidth(prompt.mLabel) + 12.0f);
            UIStack parts = UIStack::Row(strip.Last(capWidth + 6.0f + TextWidth(text)), 6.0f);
            const UIRect cap = parts.Next(capWidth);
            const UIRect caption = parts.Rest();

            ui.DrawList().Rect(cap, UIColor{0.9f, 0.9f, 0.9f, 0.9f}, prompt.mGamepad ? kPromptHeight * 0.5f : 3.0f);
            ui.DrawList().Text(cap.x + 6.0f, cap.y + 4.0f, prompt.mLabel, UIColor{0.0f, 0.0f, 0.0f, 1.0f},
                               style.mFontSize);
            ui.DrawList().Text(caption.x, caption.y + 4.0f, text, style.mTextColor, style.mFontSize);
        }
    } // namespace

    UIDemoLayout UIDemoScreen::Layout(const UIRect &safeArea)
    {
        UIDemoLayout layout;

        UIStack screen = UIStack::Column(Inset(safeArea, kMargin), kGap);
        layout.mPrompts = screen.Last(kPromptHeight);
        const UIRect above = screen.Rest();

        const float width = std::min(std::clamp(above.width * kMenuShare, kMenuMinWidth, kMenuMaxWidth), above.width);
        layout.mPanel = Anchor(above, kUILeft, width, kMenuHeight);

        UIStack menu = UIStack::Column(Inset(layout.mPanel, kPadding), kGap);
        layout.mTitle = menu.Next(kRow);
        layout.mButton = menu.Next(kRow);
        layout.mToggle = menu.Next(kRow);
        layout.mDisabled = menu.Next(kRow);

        UIStack sliderRow = UIStack::Row(menu.Next(kSliderRow), kGap);
        layout.mVolume = sliderRow.Last(kVolumeWidth);
        layout.mSlider = sliderRow.Rest();

        layout.mBar = menu.Next(kBarRow);

        UIStack imageRow = UIStack::Row(menu.Next(kRow), kGap);
        layout.mImage = imageRow.Next(kRow);
        layout.mCaption = imageRow.Rest();
        return layout;
    }

    void UIDemoScreen::Load(UIContext &ui, AssetCache *assets)
    {
        if (assets == nullptr)
            return;

        UITextures &textures = ui.Textures();
        const auto load = [&](std::string_view path) { return textures.Load(*assets, MakeAssetId(path)); };
        mArt = Art{.mPanel = Sliced(load("assets/ui/panel.png"), 16.0f),
                   .mButton = Sliced(load("assets/ui/button.png"), 8.0f),
                   .mButtonHovered = Sliced(load("assets/ui/button_hover.png"), 8.0f),
                   .mButtonHeld = Sliced(load("assets/ui/button_held.png"), 8.0f),
                   .mTrack = Sliced(load("assets/ui/track.png"), 6.0f),
                   .mFill = Sliced(load("assets/ui/fill.png"), 6.0f),
                   .mFocus = Sliced(load("assets/ui/focus.png"), 6.0f),
                   .mIcon = load("assets/ui/icon.png")};
    }

    void UIDemoScreen::Draw(UIContext &ui)
    {

        if (!mOpened)
        {
            ui.State().SetFocus(kButton);
            mOpened = true;
        }

        const bool art = mToggled;
        UIStyle &style = ui.Style();
        style.mPanelSprite = art ? mArt.mPanel : UISprite{};
        style.mWidgetSprite = art ? mArt.mButton : UISprite{};
        style.mWidgetHoveredSprite = art ? mArt.mButtonHovered : UISprite{};
        style.mWidgetHeldSprite = art ? mArt.mButtonHeld : UISprite{};
        style.mTrackSprite = art ? mArt.mTrack : UISprite{};
        style.mFillSprite = art ? mArt.mFill : UISprite{};
        style.mFocusSprite = art ? mArt.mFocus : UISprite{};

        const UIDemoLayout layout = Layout(ui.SafeArea());
        Panel(ui, layout.mPanel);

        Label(ui, UIId("demo_title"), layout.mTitle, "gameui widget demo (--ui-demo)");

        const std::string clicks = std::format("Button, clicked {} times", mClicks);
        if (Button(ui, kButton, layout.mButton, clicks, {.mLinks = {kSlider, kToggle, UIId{}, UIId{}}}).mClicked)
        {
            ++mClicks;
            MIR_LOG_INFO("gameui demo: button clicked");
        }

        if (Toggle(ui, kToggle, layout.mToggle, "Art (style sprites)", mToggled,
                   {.mLinks = {kButton, kDisabled, UIId{}, UIId{}}}).mChanged)
            MIR_LOG_INFO("gameui demo: art {}", mToggled ? "on" : "off");

        Button(ui, kDisabled, layout.mDisabled, "Disabled, focus passes over it",
               {.mLinks = {kToggle, kSlider, UIId{}, UIId{}}, .mDisabled = true});

        Slider(ui, kSlider, layout.mSlider, mVolume, 0.0f, 1.0f, 0.05f, {.mLinks = {kDisabled, kButton, UIId{}, UIId{}}});
        const std::string volume = std::format("{}%", static_cast<int>(std::lround(mVolume * 100.0f)));
        Label(ui, UIId("demo_volume"), layout.mVolume, volume);

        Bar(ui, UIId("demo_bar"), layout.mBar, mVolume);

        Image(ui, UIId("demo_image"), layout.mImage, mArt.mIcon,
              mToggled ? UIColor{0.4f, 0.8f, 0.4f, 1.0f} : UIColor{0.5f, 0.5f, 0.5f, 1.0f});
        Label(ui, UIId("demo_image_caption"), layout.mCaption, "Image, tinted by the toggle");

        UIStack prompts = UIStack::Row(layout.mPrompts, kPromptGap);
        DrawPrompt(ui, prompts, kUICancelAction, "Back");
        DrawPrompt(ui, prompts, kUIConfirmAction, "Select");

        if (ui.CancelPressed())
            MIR_LOG_INFO("gameui demo: cancelled");
    }
} // namespace mir
