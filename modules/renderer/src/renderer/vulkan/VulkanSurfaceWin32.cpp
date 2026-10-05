/**
 * @file VulkanSurfaceWin32.cpp
 * @author Sumin Park
 * @brief Window specification for Vulkan Surface
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "VulkanSurface.h"
#include <core/log/Log.h>

namespace mir::vk
{
    const char *PlatformSurfaceExtension(WindowBackend backend)
    {
        return backend == WindowBackend::Win32
                   ? VK_KHR_WIN32_SURFACE_EXTENSION_NAME
                   : nullptr;
    }

    VkSurfaceKHR CreateSurface(VkInstance instance, const NativeWindowHandle &handle)
    {
        if (handle.backend != WindowBackend::Win32)
        {
            MIR_LOG_CRITICAL("CreateSurface: backend is not Win32");
            return VK_NULL_HANDLE;
        }

        VkWin32SurfaceCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = nullptr,
            .hwnd = static_cast<HWND>(handle.window)};

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return surface;
    }
}