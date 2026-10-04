/**
 * @file InputSystem.h
 * @author Rahul Nair
 * @brief Resolves the window's RawInputSnapshot against the World's InputMap
 *        into its InputState, once per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/System.h>

namespace mir
{
    class Window;

    // Registered in SystemPhase::PreUpdate, before ScriptSystem - scripts
    // must see this frame's settled InputState, not last frame's. Requires
    // an InputMap and an InputState already emplaced on the World (see
    // App::Initialize); this system only ever reads the former and writes
    // the latter, never emplaces either itself.
    class InputSystem final : public ISystem
    {
    public:
        explicit InputSystem(Window &window) : mWindow(window) {}

        void OnUpdate(SystemContext &context) override;

    private:
        Window &mWindow;

        // the cursor as of the last update, so moving it alone makes the
        // mouse the active device
        double mLastMouseX = 0.0;
        double mLastMouseY = 0.0;
        bool mHasLastMouse = false;
    };
}
