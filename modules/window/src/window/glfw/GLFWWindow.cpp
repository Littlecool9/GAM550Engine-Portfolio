/**
 * @file GLFWWindow.cpp
 * @author Sumin Park
 * @brief GLFW window for desktop platforms
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GLFWWindow.h"

#include "core/log/Assert.h"
#include "core/log/Log.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>

namespace mir
{
    namespace
    {
        // GLFW is a process-wide singleton, so init/terminate is refcounted
        // against the number of live windows.
        int g_windowCount = 0;

        void OnGLFWError(int code, const char *description)
        {
            MIR_LOG_ERROR("GLFW error {}: {}", code, description);
        }


        bool WindowPositionKnown()
        {
            return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
        }

        GLFWmonitor *MonitorUnderWindow(GLFWwindow *handle)
        {
            GLFWmonitor *best = glfwGetPrimaryMonitor();
            if (!WindowPositionKnown())
                return best;

            int wx = 0;
            int wy = 0;
            int ww = 0;
            int wh = 0;
            glfwGetWindowPos(handle, &wx, &wy);
            glfwGetWindowSize(handle, &ww, &wh);

            int count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&count);
            int64_t bestArea = 0;
            for (int i = 0; i < count; ++i)
            {
                const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
                if (mode == nullptr)
                    continue;

                int mx = 0;
                int my = 0;
                glfwGetMonitorPos(monitors[i], &mx, &my);

                const int overlapW = std::min(wx + ww, mx + mode->width) - std::max(wx, mx);
                const int overlapH = std::min(wy + wh, my + mode->height) - std::max(wy, my);
                if (overlapW <= 0 || overlapH <= 0)
                    continue;

                const int64_t area = static_cast<int64_t>(overlapW) * overlapH;
                if (area > bestArea)
                {
                    bestArea = area;
                    best = monitors[i];
                }
            }
            return best;
        }

        static_assert(static_cast<int>(Key::Space) == GLFW_KEY_SPACE);
        static_assert(static_cast<int>(Key::A) == GLFW_KEY_A);
        static_assert(static_cast<int>(Key::Z) == GLFW_KEY_Z);
        static_assert(static_cast<int>(Key::Num0) == GLFW_KEY_0);
        static_assert(static_cast<int>(Key::F1) == GLFW_KEY_F1);
        static_assert(static_cast<int>(Key::F25) == GLFW_KEY_F25);
        static_assert(static_cast<int>(Key::Keypad0) == GLFW_KEY_KP_0);
        static_assert(static_cast<int>(Key::KeypadEqual) == GLFW_KEY_KP_EQUAL);
        static_assert(static_cast<int>(Key::LeftShift) == GLFW_KEY_LEFT_SHIFT);
        static_assert(static_cast<int>(Key::Menu) == GLFW_KEY_MENU);
        static_assert(kKeyCount == GLFW_KEY_LAST + 1);
        static_assert(static_cast<int>(MouseButton::Left) == GLFW_MOUSE_BUTTON_LEFT);
        static_assert(static_cast<int>(MouseButton::Button8) == GLFW_MOUSE_BUTTON_8);
        static_assert(kMouseButtonCount == GLFW_MOUSE_BUTTON_LAST + 1);
        static_assert(static_cast<int>(GamepadButton::A) == GLFW_GAMEPAD_BUTTON_A);
        static_assert(static_cast<int>(GamepadButton::DpadLeft) == GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
        static_assert(kGamepadButtonCount == GLFW_GAMEPAD_BUTTON_LAST + 1);
        static_assert(static_cast<int>(GamepadAxis::LeftX) == GLFW_GAMEPAD_AXIS_LEFT_X);
        static_assert(static_cast<int>(GamepadAxis::RightTrigger) == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER);
        static_assert(kGamepadAxisCount == GLFW_GAMEPAD_AXIS_LAST + 1);
        static_assert(kGamepadSlotCount == GLFW_JOYSTICK_LAST + 1);
    }

    std::unique_ptr<Window> Window::Create(const WindowDesc &desc)
    {
        return std::make_unique<GLFWWindow>(desc);
    }

    GLFWWindow::GLFWWindow(const WindowDesc &desc)
    {
        if (g_windowCount == 0)
        {
            glfwSetErrorCallback(&OnGLFWError);
            MIR_CHECK(glfwInit() == GLFW_TRUE, "glfwInit failed");
        }

        // No OpenGL context. The renderer owns the graphics API.
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, desc.mResizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_MAXIMIZED, desc.mMaximized ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, desc.mStartHidden ? GLFW_FALSE : GLFW_TRUE);

        mHandle = glfwCreateWindow(static_cast<int>(desc.mWidth),
                                    static_cast<int>(desc.mHeight),
                                    desc.mTitle,
                                    nullptr,
                                    nullptr);
        MIR_CHECK(mHandle != nullptr, "glfwCreateWindow failed");
        ++g_windowCount;

        mTitle = desc.mTitle;
        mHasCustomTitleBar = desc.mCustomTitleBar;
        if (mHasCustomTitleBar)
            InstallCustomTitleBar();

        RefreshFramebufferSize();

        // Lets the static callbacks recover the owning instance.
        glfwSetWindowUserPointer(mHandle, this);
        glfwSetFramebufferSizeCallback(mHandle, &GLFWWindow::OnFramebufferSize);
        glfwSetScrollCallback(mHandle, &GLFWWindow::OnScroll);
        glfwSetDropCallback(mHandle, &GLFWWindow::OnDrop);

        MIR_LOG_INFO("Window created: {}x{} \"{}\"", mWidth, mHeight, desc.mTitle);
    }

    GLFWWindow::~GLFWWindow()
    {
        if (mHandle != nullptr)
        {
            if (mHasCustomTitleBar)
                UninstallCustomTitleBar();

            glfwDestroyWindow(mHandle);
            mHandle = nullptr;
            --g_windowCount;
        }

        if (g_windowCount == 0)
        {
            glfwTerminate();
        }
    }

    void GLFWWindow::PollEvents()
    {
        mDroppedFiles.clear();
        mRawInput.scrollX = 0.0;
        mRawInput.scrollY = 0.0;

        glfwPollEvents();

        for (Key key : AllKeys())
            mRawInput.keys[static_cast<size_t>(key)] = glfwGetKey(mHandle, static_cast<int>(key)) == GLFW_PRESS;

        for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; ++button)
            mRawInput.mouseButtons[static_cast<size_t>(button)] = glfwGetMouseButton(mHandle, button) == GLFW_PRESS;

        glfwGetCursorPos(mHandle, &mRawInput.mouseX, &mRawInput.mouseY);

        for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid)
        {
            GamepadState &pad = mRawInput.gamepads[static_cast<size_t>(jid)];

            GLFWgamepadstate state{};
            if (glfwGetGamepadState(jid, &state) != GLFW_TRUE)
            {
                pad.connected = false;
                continue;
            }

            pad.connected = true;
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
                pad.buttons[static_cast<size_t>(b)] = state.buttons[b] == GLFW_PRESS;
            for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a)
                pad.axes[static_cast<size_t>(a)] = state.axes[a];
        }
    }

    bool GLFWWindow::ShouldClose() const
    {
        return glfwWindowShouldClose(mHandle) == GLFW_TRUE;
    }

    float GLFWWindow::ContentScale() const
    {
        float xscale = 1.0f;
        float yscale = 1.0f;
        glfwGetWindowContentScale(mHandle, &xscale, &yscale);
        return xscale;
    }

    void GLFWWindow::Minimize()
    {
        glfwIconifyWindow(mHandle);
    }

    void GLFWWindow::ToggleMaximize()
    {
        if (glfwGetWindowAttrib(mHandle, GLFW_MAXIMIZED) == GLFW_TRUE)
            glfwRestoreWindow(mHandle);
        else
            glfwMaximizeWindow(mHandle);
    }

    bool GLFWWindow::IsMaximized() const
    {
        return glfwGetWindowAttrib(mHandle, GLFW_MAXIMIZED) == GLFW_TRUE;
    }

    void GLFWWindow::RequestClose()
    {
        glfwSetWindowShouldClose(mHandle, GLFW_TRUE);
    }
    void GLFWWindow::Show()
    {
        glfwShowWindow(mHandle);
    }

    void GLFWWindow::ToggleFullscreen()
    {
        if (IsFullscreen())
        {
            glfwSetWindowMonitor(mHandle, nullptr, mWindowedX, mWindowedY, mWindowedWidth, mWindowedHeight,
                                 GLFW_DONT_CARE);
            if (mHasCustomTitleBar)
                PlaceCustomTitleBarWindow(mWindowedX, mWindowedY, mWindowedWidth, mWindowedHeight);
            if (mWindowedMaximized)
                glfwMaximizeWindow(mHandle);
        }
        else
        {

            GLFWmonitor *monitor = MonitorUnderWindow(mHandle);
            const GLFWvidmode *mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
            if (mode == nullptr)
            {
                MIR_LOG_WARN("ToggleFullscreen: no monitor to go fullscreen on, staying windowed");
                return;
            }

            mWindowedMaximized = IsMaximized();
            if (mWindowedMaximized)
                glfwRestoreWindow(mHandle);

            if (WindowPositionKnown())
                glfwGetWindowPos(mHandle, &mWindowedX, &mWindowedY);
            glfwGetWindowSize(mHandle, &mWindowedWidth, &mWindowedHeight);

            //the monitor's current mode, so the display does not switch modes
            glfwSetWindowMonitor(mHandle, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }

        RefreshFramebufferSize();
        MIR_LOG_INFO("Window {}: {}x{}", IsFullscreen() ? "fullscreen" : "windowed", mWidth, mHeight);
    }

    bool GLFWWindow::IsFullscreen() const
    {
        return glfwGetWindowMonitor(mHandle) != nullptr;
    }

    void GLFWWindow::RefreshFramebufferSize()
    {
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(mHandle, &fbWidth, &fbHeight);
        mWidth = static_cast<uint32_t>(fbWidth);
        mHeight = static_cast<uint32_t>(fbHeight);
    }

    void GLFWWindow::OnFramebufferSize(GLFWwindow *handle, int width, int height)
    {
        auto *self = static_cast<GLFWWindow *>(glfwGetWindowUserPointer(handle));
        MIR_ASSERT(self != nullptr, "framebuffer callback without a user pointer");

        // Minimizing reports 0x0. Keep it; the renderer decides to skip frames.
        self->mWidth = static_cast<uint32_t>(width);
        self->mHeight = static_cast<uint32_t>(height);
    }

    void GLFWWindow::OnScroll(GLFWwindow *handle, double xoffset, double yoffset)
    {
        auto *self = static_cast<GLFWWindow *>(glfwGetWindowUserPointer(handle));
        MIR_ASSERT(self != nullptr, "scroll callback without a user pointer");

        self->mRawInput.scrollX += xoffset;
        self->mRawInput.scrollY += yoffset;
    }

    void GLFWWindow::OnDrop(GLFWwindow *handle, int count, const char **paths)
    {
        auto *self = static_cast<GLFWWindow *>(glfwGetWindowUserPointer(handle));
        MIR_ASSERT(self != nullptr, "drop callback without a user pointer");

        // GLFW owns the strings only for the duration of this call.
        for (int i = 0; i < count; ++i)
            self->mDroppedFiles.emplace_back(paths[i]);
    }
}
