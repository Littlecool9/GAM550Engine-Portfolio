/**
 * @file VulkanSurfaceLinux.cpp
 * @author Sumin Park
 * @brief Window specification for Vulkan Surface
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "VulkanSurface.h"
#include <core/log/Log.h>

struct wl_display;
struct wl_surface;

namespace mir::vk
{
    const char *PlatformSurfaceExtension(WindowBackend backend)
    {
        switch (backend)
        {
        case WindowBackend::Xlib:
            return VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
        case WindowBackend::Wayland:
            return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
        default:
            return nullptr;
        }
    }

    VkSurfaceKHR CreateSurface(VkInstance instance, const NativeWindowHandle &handle)
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;

        if (handle.backend == WindowBackend::Wayland)
        {
            VkWaylandSurfaceCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                .display = static_cast<wl_display *>(handle.display),
                .surface = static_cast<wl_surface *>(handle.window)};

            result = vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface);
        }
        else if (handle.backend == WindowBackend::Xlib)
        {
            VkXlibSurfaceCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                .dpy = static_cast<Display *>(handle.display),
                .window = static_cast<::Window>(reinterpret_cast<uintptr_t>(handle.window))};

            result = vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface);
        }
        else
        {
            MIR_LOG_ERROR("CreateSurface: Unsupported window backend for Vulkan surface creation");
            return VK_NULL_HANDLE;
        }

        if (result != VK_SUCCESS)
        {
            MIR_LOG_ERROR("CreateSurface: Vulkan surface creation failed");
            return VK_NULL_HANDLE;
        }
        return surface;
    }
}