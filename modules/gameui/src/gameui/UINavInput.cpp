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
} // namespace mir
