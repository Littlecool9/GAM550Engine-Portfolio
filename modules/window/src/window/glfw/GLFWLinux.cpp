/**
 * @file GLFWLinux.cpp
 * @author Sumin Park
 * @brief Linux specifics for GLFW
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "GLFWWindow.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>

namespace mir
{
    void EnsureDpiAware() {}

    NativeWindowHandle GLFWWindow::NativeWindow() const
    {
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        {
            return {WindowBackend::Wayland, glfwGetWaylandDisplay(), glfwGetWaylandWindow(mHandle)};
        }
        // X11 Window ids are unsigned long, need to change to pointers
        return {WindowBackend::Xlib, glfwGetX11Display(), reinterpret_cast<void *>(static_cast<uintptr_t>(glfwGetX11Window(mHandle)))};
    }
    void GLFWWindow::InstallCustomTitleBar() {}
    void GLFWWindow::UninstallCustomTitleBar() {}
    void GLFWWindow::PlaceCustomTitleBarWindow(int, int, int, int) {}
}