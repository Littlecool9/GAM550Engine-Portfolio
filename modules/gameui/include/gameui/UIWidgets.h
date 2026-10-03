/**
 * @file UIWidgets.h
 * @author Quy Dong Ta
 * @brief The core widgets, Label, Button, Image, Bar, Toggle and Slider: each
 *        declares itself through UIContext::Interact, draws its parts as the
 *        style's sprites or, without them, grey-box rects, draws its text,
 *        and returns what happened to it this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIContext.h"
#include "UIId.h"
#include "UIState.h"
#include "UITypes.h"

#include <string_view>

namespace mir
{

    struct UIWidgetResult : UIInteraction
    {
        bool mChanged = false;
    };

    struct UIDisplayOptions
    {
        UIWidgetOptions mWidget;
        bool mFocusable = false;
    };

    void Panel(UIContext &ui, const UIRect &rect);

    UIWidgetResult Label(UIContext &ui, UIId id, const UIRect &rect, std::string_view text,
                         const UIDisplayOptions &options = {});

    UIWidgetResult Button(UIContext &ui, UIId id, const UIRect &rect, std::string_view text,
                          const UIWidgetOptions &options = {});


    UIWidgetResult Image(UIContext &ui, UIId id, const UIRect &rect, const UISprite &sprite, UIColor tint = kUIWhite,
                         const UIDisplayOptions &options = {});

    //fraction of the bar filled, clamped to 0 to 1
    UIWidgetResult Bar(UIContext &ui, UIId id, const UIRect &rect, float fraction,
                       const UIDisplayOptions &options = {});

    UIWidgetResult Toggle(UIContext &ui, UIId id, const UIRect &rect, std::string_view text, bool &value,
                          const UIWidgetOptions &options = {});

    UIWidgetResult Slider(UIContext &ui, UIId id, const UIRect &rect, float &value, float min, float max,
                          float step = 0.0f, const UIWidgetOptions &options = {});
} // namespace mir
