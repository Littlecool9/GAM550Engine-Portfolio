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
}
