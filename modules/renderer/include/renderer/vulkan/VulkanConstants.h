/**
 * @file VulkanConstants.h
 * @author Sumin Park
 * @brief Constants shared by the Vulkan renderer and its subsystems.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>

namespace mir::vk
{
    constexpr uint32_t kFramesInFlight = 2;

    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    // Sizes the per-view slices of the camera, light and spot matrix buffers.
    constexpr uint32_t kMaxViewsPerFrame = 16;

    constexpr uint32_t kMaxSpotShadows = 4;
    constexpr uint32_t kMaxPointShadows = 2;
}
