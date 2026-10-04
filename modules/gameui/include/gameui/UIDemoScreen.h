/**
 * @file UIDemoScreen.h
 * @author Quy Dong Ta
 * @brief A debug screen that exercises every core widget, shown by App under
 *        --ui-demo, in grey box or in the art under assets/ui/. It holds the
 *        values its widgets edit, as any screen would.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIContext.h"
#include "UITypes.h"

#include <cstdint>

namespace mir
{
    class AssetCache;


    struct UIDemoLayout
    {
        UIRect mPanel;
        UIRect mTitle;
        UIRect mButton;
        UIRect mToggle;
        UIRect mDisabled;
        UIRect mSlider;
        UIRect mVolume;
        UIRect mBar;
        UIRect mImage;
        UIRect mCaption;

        UIRect mPrompts;
    };

    class UIDemoScreen
    {
    public:

        static UIDemoLayout Layout(const UIRect &safeArea);


        void Load(UIContext &ui, AssetCache *assets);

        // between UIContext::BeginFrame and EndFrame
        void Draw(UIContext &ui);

        uint32_t Clicks() const { return mClicks; }
        bool Toggled() const { return mToggled; }
        float Volume() const { return mVolume; }

    private:
        // the style's sprites while the toggle is on; null until Load
        struct Art
        {
            UISprite mPanel;
            UISprite mButton;
            UISprite mButtonHovered;
            UISprite mButtonHeld;
            UISprite mTrack;
            UISprite mFill;
            UISprite mFocus;
            UISprite mIcon;
        };

        Art mArt;
        uint32_t mClicks = 0;
        bool mToggled = true;
        float mVolume = 0.5f;
        bool mOpened = false;
    };
} // namespace mir
