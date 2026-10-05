/**
 * @file FrameSync.cpp
 * @author Sumin Park
 * @brief Frame loop shared by the raster and ray traced paths: wait, acquire, submit, present.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "FrameSync.h"
#include "VulkanDevice.h"
#include "VulkanSwapchain.h"

#include <core/log/Log.h>

namespace mir::vk
{
    bool FrameSync::Initialize(VulkanDevice &device)
    {
        mDevice = &device;

        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = 0,
            .queueFamilyIndex = mDevice->QueueFamily() };

        const VkSemaphoreCreateInfo semInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (vkCreateCommandPool(mDevice->Handle(), &poolInfo, nullptr, &mCmdPools[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateCommandPool failed for frame {}", i);
                return false;
            }

            const VkCommandBufferAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = mCmdPools[i],
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1 };

            if (vkAllocateCommandBuffers(mDevice->Handle(), &allocInfo, &mCmdBuffers[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkAllocateCommandBuffers failed for frame {}", i);
                return false;
            }

            if (vkCreateSemaphore(mDevice->Handle(), &semInfo, nullptr, &mImageAcquired[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("image-acquired semaphore failed for frame {}", i);
                return false;
            }
        }

        // Paired with mNextSignalValue = kFramesInFlight + 1
        const VkSemaphoreTypeCreateInfo typeInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = kFramesInFlight };

        const VkSemaphoreCreateInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &typeInfo };

        if (vkCreateSemaphore(mDevice->Handle(), &timelineInfo, nullptr, &mTimeline) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("timeline semaphore creation failed");
            return false;
        }

        return true;
    }

    void FrameSync::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        if (mTimeline != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(mDevice->Handle(), mTimeline, nullptr);
            mTimeline = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mImageAcquired[i] != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(mDevice->Handle(), mImageAcquired[i], nullptr);
                mImageAcquired[i] = VK_NULL_HANDLE;
            }
            // Destroying the pool frees its command buffers too.
            if (mCmdPools[i] != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(mDevice->Handle(), mCmdPools[i], nullptr);
                mCmdPools[i] = VK_NULL_HANDLE;
                mCmdBuffers[i] = VK_NULL_HANDLE;
            }
        }
    }

    std::optional<FrameContext> FrameSync::Begin(VulkanSwapchain &swapchain)
    {
        const uint64_t signalValue = mNextSignalValue++;
        const uint64_t waitValue = signalValue - kFramesInFlight;

        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &mTimeline,
            .pValues = &waitValue };

        // wait for next image to comeback from framesin flight
        vkWaitSemaphores(mDevice->Handle(), &waitInfo, UINT64_MAX);

        // Whole-pool reset, not per-buffer.
        vkResetCommandPool(mDevice->Handle(), mCmdPools[mFrameIndex], 0);

        uint32_t imageIndex = 0;
        if (!swapchain.Acquire(mImageAcquired[mFrameIndex], imageIndex))
        {
            // Roll the signal value back: nothing will signal it.
            --mNextSignalValue;
            return std::nullopt;
        }

        VkCommandBuffer cmd = mCmdBuffers[mFrameIndex];

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

        vkBeginCommandBuffer(cmd, &beginInfo);

        return FrameContext{ .cmd = cmd, .imageIndex = imageIndex, .signalValue = signalValue };
    }

    void FrameSync::End(VulkanSwapchain &swapchain, const FrameContext &frame)
    {
        vkEndCommandBuffer(frame.cmd);

        const VkSemaphoreSubmitInfo waitSem{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mImageAcquired[mFrameIndex],
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };

        const VkSemaphoreSubmitInfo signalSems[]{
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = swapchain.RenderComplete(frame.imageIndex),
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = mTimeline,
             .value = frame.signalValue,
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT } };

        const VkCommandBufferSubmitInfo cmdInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame.cmd };

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitSem,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos = signalSems };

        if (vkQueueSubmit2(mDevice->Queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkQueueSubmit2 failed");
            return;
        }

        swapchain.Present(frame.imageIndex);

        mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
    }
}
