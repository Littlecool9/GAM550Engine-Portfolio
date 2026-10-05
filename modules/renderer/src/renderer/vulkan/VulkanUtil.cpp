/**
 * @file VulkanUtil.cpp
 * @author Sumin Park
 * @brief Vulkan helpers shared across the renderer's source files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "VulkanUtil.h"
#include "Device.h"

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace mir::vk
{
    namespace
    {
        std::optional<std::vector<uint32_t>> ReadSpirv(const std::filesystem::path &path)
        {
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
            if (!bytes)
                // ReadBytes takes care of logging onfailure
                return std::nullopt;

            if (bytes->empty() || bytes->size() % sizeof(uint32_t) != 0)
            {
                MIR_LOG_CRITICAL("Bad SPIR-V size ({} bytes): {}", bytes->size(), path.string());
                return std::nullopt;
            }

            // Copied, not reinterpreted: vkCreateShaderModule needs pCode 4-byte
            // aligned and a vector<byte> is only aligned to 1.
            std::vector<uint32_t> words(bytes->size() / sizeof(uint32_t));
            std::memcpy(words.data(), bytes->data(), bytes->size());
            return words;
        }
    }

    bool CreateBuffer(VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaAllocationCreateFlags allocFlags,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation,
        VmaAllocationInfo *outInfo)
    {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };

        const VmaAllocationCreateInfo allocInfo{
            .flags = allocFlags,
            .usage = VMA_MEMORY_USAGE_AUTO };

        return vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
            &outBuffer, &outAllocation, outInfo) == VK_SUCCESS;
    }

    bool CreateUploadBuffer(VmaAllocator allocator,
        std::span<const std::byte> bytes,
        VkBufferUsageFlags usage,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation)
    {
        VmaAllocationInfo info{};
        if (!CreateBuffer(allocator, bytes.size(), usage,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            outBuffer, outAllocation, &info))
            return false;

        std::memcpy(info.pMappedData, bytes.data(), bytes.size());
        vmaFlushAllocation(allocator, outAllocation, 0, VK_WHOLE_SIZE);
        return true;
    }

    VkRect2D ResolveViewRect(const VkRect2D &requested, VkExtent2D swapchainExtent)
    {
        VkRect2D rect = (requested.extent.width > 0 && requested.extent.height > 0)
            ? requested
            : VkRect2D{ .offset{ 0, 0 }, .extent = swapchainExtent };

        rect.offset.x = std::clamp(rect.offset.x, 0, static_cast<int32_t>(swapchainExtent.width));
        rect.offset.y = std::clamp(rect.offset.y, 0, static_cast<int32_t>(swapchainExtent.height));
        rect.extent.width = std::min(rect.extent.width,
            swapchainExtent.width - static_cast<uint32_t>(rect.offset.x));
        rect.extent.height = std::min(rect.extent.height,
            swapchainExtent.height - static_cast<uint32_t>(rect.offset.y));
        return rect;
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        uint32_t levelCount, uint32_t layerCount)
    {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStage,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspectMask,
                .levelCount = levelCount,
                .layerCount = layerCount } };

        const VkDependencyInfo dep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier };

        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path &path)
    {
        const std::optional<std::vector<uint32_t>> words = ReadSpirv(path);
        if (!words.has_value())
            return VK_NULL_HANDLE;

        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words->size() * sizeof(uint32_t),
            .pCode = words->data() };

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateShaderModule failed: {}", path.string());
            return VK_NULL_HANDLE;
        }
        return module;
    }

    bool SubmitAndWait(const Device &device, const std::function<void(VkCommandBuffer)> &record)
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = device.QueueFamily() };

        if (vkCreateCommandPool(device.Handle(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("SubmitAndWait: command pool creation failed");
            return false;
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        vkAllocateCommandBuffers(device.Handle(), &allocInfo, &cmd);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &beginInfo);
        record(cmd);
        vkEndCommandBuffer(cmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd };

        const bool submitted = vkQueueSubmit(device.Queue(), 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
        if (submitted)
            vkQueueWaitIdle(device.Queue());
        else
            MIR_LOG_CRITICAL("SubmitAndWait: vkQueueSubmit failed");

        // Destroying the pool frees its command buffer too.
        vkDestroyCommandPool(device.Handle(), pool, nullptr);
        return submitted;
    }
}
