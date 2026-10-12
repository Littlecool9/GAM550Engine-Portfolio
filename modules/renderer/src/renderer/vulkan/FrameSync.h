/**
 * @file FrameSync.h
 * @author Sumin Park
 * @brief Per-frame command pools and buffers, image-acquired semaphores and the frame timeline.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <optional>

namespace mir::vk
{
    class Device;
    class Swapchain;

    struct FrameContext
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
        uint64_t signalValue = 0;
    };

    class FrameSync
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        // Waits for this frame slot, acquires an image and begins its command buffer.
        // nullopt when acquire fails or Stopped(); the frame is skipped and nothing was submitted.
        std::optional<FrameContext> Begin(Swapchain &swapchain);

        // False when the submit failed and nothing was presented.
        bool End(Swapchain &swapchain, const FrameContext &frame);

        uint32_t Index() const { return mFrameIndex; }

        // True once a submit or wait failed; no frame begins after that.
        bool Stopped() const { return mStopped; }

        // The timeline value of the last frame begun: work recorded so far is done once it completes.
        uint64_t LastIssuedValue() const { return mNextSignalValue - 1; }
        // Every frame at or below it has finished on the GPU; does not wait.
        uint64_t CompletedValue() const;

    private:
        Device *mDevice = nullptr;

        VkCommandPool mCmdPools[kFramesInFlight]{};
        VkCommandBuffer mCmdBuffers[kFramesInFlight]{};
        VkSemaphore mImageAcquired[kFramesInFlight]{};

        VkSemaphore mTimeline = VK_NULL_HANDLE;

        // waitvalue = signalValue - kFramesInFlight
        // dont start at 0 or 1 or else underflows
        uint64_t mNextSignalValue = kFramesInFlight + 1;

        uint32_t mFrameIndex = 0;
        bool mStopped = false;
    };
}
