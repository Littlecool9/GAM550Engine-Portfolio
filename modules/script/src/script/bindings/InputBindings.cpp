/**
 * @file InputBindings.cpp
 * @author Rahul Nair
 * @brief Extends the World usertype with world:is_down/world:axis/etc.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "InputBindings.h"

#include <core/ecs/World.h>
#include <input/InputState.h>

#include <sol/sol.hpp>

#include <string_view>
#include <tuple>

namespace mir
{
    namespace
    {
        bool WorldIsActionDown(World &world, std::string_view action)
        {
            const InputState *state = world.TryResource<InputState>();
            return state != nullptr && state->IsDown(action);
        }

        bool WorldJustPressed(World &world, std::string_view action)
        {
            const InputState *state = world.TryResource<InputState>();
            return state != nullptr && state->JustPressed(action);
        }

        bool WorldJustReleased(World &world, std::string_view action)
        {
            const InputState *state = world.TryResource<InputState>();
            return state != nullptr && state->JustReleased(action);
        }

        float WorldActionAxis(World &world, std::string_view action)
        {
            const InputState *state = world.TryResource<InputState>();
            return state != nullptr ? state->Axis(action) : 0.0f;
        }

        std::tuple<float, float> WorldActionAxis2D(World &world, std::string_view action)
        {
            const InputState *state = world.TryResource<InputState>();
            if (state == nullptr)
                return {0.0f, 0.0f};

            const ActionValue &value = state->Get(action);
            return {value.x, value.y};
        }

        std::string_view WorldActiveDevice(World &world)
        {
            const InputState *state = world.TryResource<InputState>();
            if (state == nullptr)
                return "Keyboard";

            switch (state->ActiveDevice())
            {
            case DeviceKind::Keyboard:
                return "Keyboard";
            case DeviceKind::Mouse:
                return "Mouse";
            case DeviceKind::Gamepad:
                return "Gamepad";
            }
            return "Keyboard";
        }
    }

    void RegisterInputBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("is_down", &WorldIsActionDown);
        worldType.set_function("just_pressed", &WorldJustPressed);
        worldType.set_function("just_released", &WorldJustReleased);
        worldType.set_function("axis", &WorldActionAxis);
        worldType.set_function("axis2d", &WorldActionAxis2D);
        worldType.set_function("active_device", &WorldActiveDevice);
    }
}
