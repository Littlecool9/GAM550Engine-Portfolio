/**
 * @file VulkanUtil.h
 * @author Sumin Park
 * @brief Vulkan helpers shared across the renderer's source files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <filesystem>

namespace mir::vk
{
    bool CreateBuffer(VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaAllocationCreateFlags allocFlags,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation,
        VmaAllocationInfo *outInfo);

    // An empty requested rect means the whole swapchain.
    VkRect2D ResolveViewRect(const VkRect2D &requested, VkExtent2D swapchainExtent);

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        uint32_t levelCount = 1, uint32_t layerCount = 1);

    // VK_NULL_HANDLE on failure, already logged.
    VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path &path);
}
