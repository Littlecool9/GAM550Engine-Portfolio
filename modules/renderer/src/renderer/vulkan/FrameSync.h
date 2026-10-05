/**
 * @file FrameSync.h
 * @author Sumin Park
 * @brief Per-frame command pools and buffers, image-acquired semaphores and the frame timeline.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/vulkan/VulkanConstants.h>

#include <volk.h>

#include <cstdint>
#include <optional>

namespace mir::vk
{
    class VulkanDevice;
    class VulkanSwapchain;

    struct FrameContext
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
        uint64_t signalValue = 0;
    };

    class FrameSync
    {
    public:
        bool Initialize(VulkanDevice &device);
        void Shutdown();

        // Waits for this frame slot, acquires an image and begins its command buffer.
        // nullopt when acquire fails; the frame is skipped and nothing was submitted.
        std::optional<FrameContext> Begin(VulkanSwapchain &swapchain);

        // Ends, submits and presents the frame, then moves to the next slot.
        void End(VulkanSwapchain &swapchain, const FrameContext &frame);

        uint32_t Index() const { return mFrameIndex; }

    private:
        VulkanDevice *mDevice = nullptr;

        VkCommandPool mCmdPools[kFramesInFlight]{};
        VkCommandBuffer mCmdBuffers[kFramesInFlight]{};
        VkSemaphore mImageAcquired[kFramesInFlight]{};

        VkSemaphore mTimeline = VK_NULL_HANDLE;

        // waitvalue = signalValue - kFramesInFlight
        // dont start at 0 or 1 or else underflows
        uint64_t mNextSignalValue = kFramesInFlight + 1;

        uint32_t mFrameIndex = 0;
    };
}
