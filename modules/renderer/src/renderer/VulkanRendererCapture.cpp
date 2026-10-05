/**
 * @file VulkanRendererCapture.cpp
 * @author Sumin Park
 * @brief Frame capture: copy the swapchain image to a buffer and write a PNG.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <renderer/VulkanRenderer.h>
#include "vulkan/Device.h"
#include "vulkan/Swapchain.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <utility>
#include <vector>

namespace mir
{
    bool VulkanRenderer::RecordCapture(VkCommandBuffer cmd, uint32_t imageIndex)
    {
        if (mCapturePath.empty())
            return false;
        if (!mSwapchain->SupportsCapture() ||
            (mSwapchain->Format() != VK_FORMAT_B8G8R8A8_SRGB && mSwapchain->Format() != VK_FORMAT_R8G8B8A8_SRGB))
        {
            MIR_LOG_ERROR("frame capture unavailable for this surface format");
            mCapturePath.clear();
            return false;
        }
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = uint64_t(mSwapchain->Extent().width) * mSwapchain->Extent().height * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(mDevice->Allocator(), &info, &allocation, &mCaptureBuffer, &mCaptureAllocation, nullptr) !=
            VK_SUCCESS)
        {
            MIR_LOG_ERROR("capture allocation failed");
            mCapturePath.clear();
            return false;
        }
        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = mSwapchain->Image(imageIndex);
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { mSwapchain->Extent().width, mSwapchain->Extent().height, 1 };
        vkCmdCopyImageToBuffer(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mCaptureBuffer,
            1, &copy);
        VkBufferMemoryBarrier2 host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = mCaptureBuffer;
        host.size = VK_WHOLE_SIZE;
        dep.imageMemoryBarrierCount = 0;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd, &dep);
        return true;
    }
    void VulkanRenderer::FinishCapture(bool submitted)
    {
        // Unsubmitted: no GPU work references the buffer, so it can be freed without a wait.
        if (!submitted)
        {
            MIR_LOG_ERROR("frame capture dropped, the frame was not submitted: {}", mCapturePath.string());
        }
        else
        {
            vkQueueWaitIdle(mDevice->Queue());
            vmaInvalidateAllocation(mDevice->Allocator(), mCaptureAllocation, 0, VK_WHOLE_SIZE);
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(mDevice->Allocator(), mCaptureAllocation, &info);
            const auto size = size_t(mSwapchain->Extent().width) * mSwapchain->Extent().height * 4;
            const auto *begin = static_cast<const uint8_t *>(info.pMappedData);
            std::vector<uint8_t> pixels(begin, begin + size);
            if (mSwapchain->Format() == VK_FORMAT_B8G8R8A8_SRGB)
                for (size_t i = 0; i < size; i += 4)
                    std::swap(pixels[i], pixels[i + 2]);
            if (!stbi_write_png(mCapturePath.string().c_str(), int(mSwapchain->Extent().width), int(mSwapchain->Extent().height), 4,
                pixels.data(), int(mSwapchain->Extent().width * 4)))
                MIR_LOG_ERROR("cannot write frame capture: {}", mCapturePath.string());
            else
                MIR_LOG_INFO("Captured frame: {}", mCapturePath.string());
        }
        DestroyCaptureBuffer();
        mCapturePath.clear();
    }

    void VulkanRenderer::DestroyCaptureBuffer()
    {
        if (mCaptureBuffer == VK_NULL_HANDLE)
            return;
        vmaDestroyBuffer(mDevice->Allocator(), mCaptureBuffer, mCaptureAllocation);
        mCaptureBuffer = VK_NULL_HANDLE;
        mCaptureAllocation = VK_NULL_HANDLE;
    }
}
