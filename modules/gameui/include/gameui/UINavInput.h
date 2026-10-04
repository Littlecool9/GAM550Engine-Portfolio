/**
 * @file UINavInput.h
 * @author Quy Dong Ta
 * @brief The UI actions for one frame, read from InputState by action name, so
 *        no widget sees which device produced them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputState.h>

#include <cstdint>
#include <optional>

namespace mir
{

    enum class UINavDirection : uint8_t
    {
        Up,
        Down,
        Left,
        Right,
    };

    struct UINavInput
    {
        //in -1 to 1, +x is right +y is down
        float mNavigateX = 0.0f;
        float mNavigateY = 0.0f;


        bool mNavigatePressed = false;

        //each is true only on the frame its action was pressed
        bool mConfirm = false;
        bool mCancel = false;
        bool mTabLeft = false;
        bool mTabRight = false;
        bool mPause = false;
    };

    UINavInput ReadNavInput(const InputState &input);

    class UINavStepper
    {
    public:
        
        std::optional<UINavDirection> Step(const UINavInput &nav);

    private:
        
        uint8_t mHeld = 0;
    };
} // namespace mir
