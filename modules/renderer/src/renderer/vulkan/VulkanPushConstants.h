/**
 * @file VulkanPushConstants.h
 * @author Sumin Park
 * @brief CPU mirrors of the shaders' push constant blocks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace mir::vk
{
    // normalCols = Normal matrix
    // vulkan's guaranteed minimum constant size = 128
    // vulkan pads every column to 16 anyways
    struct RasterPushConstants
    {
        glm::mat4 model;      // 64
        glm::vec4 normalCol0; // 16
        glm::vec4 normalCol1; // 16
        glm::vec4 normalCol2; // 16
        glm::vec4 tint;       // 16
    };

    // Ray gen writes at outputOffset so each view traces only its own rect.
    struct RtPushConstants
    {
        glm::uvec2 outputOffset;
    };
}
