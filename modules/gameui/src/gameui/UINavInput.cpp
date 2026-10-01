/**
 * @file UINavInput.cpp
 * @author Quy Dong Ta
 * @brief The UI actions for one frame, read from InputState by action name, so
 *        no widget sees which device produced them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UINavInput.h>

#include <input/DefaultInputMap.h>

#include <cmath>

namespace mir
{
    UINavInput ReadNavInput(const InputState &input)
    {
        const ActionValue &navigate = input.Get(kUINavigateAction);

        UINavInput nav;
        nav.mNavigateX = navigate.x;
        nav.mNavigateY = navigate.y;
        nav.mNavigatePressed = navigate.justPressed;
        nav.mConfirm = input.JustPressed(kUIConfirmAction);
        nav.mCancel = input.JustPressed(kUICancelAction);
        nav.mTabLeft = input.JustPressed(kUITabLeftAction);
        nav.mTabRight = input.JustPressed(kUITabRightAction);
        return nav;
    }

    std::optional<UINavDirection> UINavStepper::Step(const UINavInput &nav)
    {
        constexpr UINavDirection kDirections[] = {UINavDirection::Up, UINavDirection::Down, UINavDirection::Left,
                                                  UINavDirection::Right};

        const float x = nav.mNavigateX;
        const float y = nav.mNavigateY;

        uint8_t held = 0;
        std::optional<UINavDirection> step;
        float stepAlong = 0.0f;
        for (const UINavDirection direction : kDirections)
        {
            float along = 0.0f;
            float across = 0.0f;
            switch (direction)
            {
            case UINavDirection::Up:
                along = -y;
                across = std::abs(x);
                break;
            case UINavDirection::Down:
                along = y;
                across = std::abs(x);
                break;
            case UINavDirection::Left:
                along = -x;
                across = std::abs(y);
                break;
            case UINavDirection::Right:
                along = x;
                across = std::abs(y);
                break;
            }

            const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(direction));
            const bool wasHeld = (mHeld & bit) != 0;
            const bool isHeld = along > 0.0f && (wasHeld ? across < 2.0f * along : across <= along);
            if (!isHeld)
                continue;

            held |= bit;

            if (!wasHeld && along > stepAlong)
            {
                step = direction;
                stepAlong = along;
            }
        }

        mHeld = held;
        return step;
    }
} // namespace mir
