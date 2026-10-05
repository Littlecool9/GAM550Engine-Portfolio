/**
 * @file Swapchain.h
 * @author Sumin Park
 * @brief Swapchain, its image views, per-frame depth images and render-complete semaphores.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir
{
    class ISurfaceProvider;
}

namespace mir::vk
{
    class Device;

    class Swapchain
    {
    public:
        bool Initialize(Device &device, const ISurfaceProvider &window);
        void Shutdown();

        // The caller idles the device first: in-flight frames still use the old images.
        bool Recreate(const ISurfaceProvider &window);

        bool NeedsRecreate(const ISurfaceProvider &window) const;

        // Forces a recreate next frame, even when the extent already matches the window.
        void MarkOutOfDate() { mOutOfDate = true; }

        // False when the frame cannot proceed: `signal` was not signalled.
        bool Acquire(VkSemaphore signal, uint32_t &outImageIndex);

        // Waits on RenderComplete(imageIndex).
        void Present(uint32_t imageIndex);

        VkSwapchainKHR Handle() const { return mSwapchain; }
        VkFormat Format() const { return mFormat; }
        VkExtent2D Extent() const { return mExtent; }

        uint32_t ImageCount() const { return static_cast<uint32_t>(mImages.size()); }
        VkImage Image(uint32_t imageIndex) const { return mImages[imageIndex]; }
        VkImageView View(uint32_t imageIndex) const { return mViews[imageIndex]; }
        VkSemaphore RenderComplete(uint32_t imageIndex) const { return mRenderComplete[imageIndex]; }

        // Indexed by frame in flight, not by swapchain image.
        VkImage DepthImage(uint32_t frame) const { return mDepthImages[frame]; }
        VkImageView DepthView(uint32_t frame) const { return mDepthViews[frame]; }

        bool SupportsCapture() const { return mCanCapture; }

    private:
        bool Create(const ISurfaceProvider &window);
        void Destroy();
        bool CreateImageViews();
        bool CreateDepthResources();
        void DestroyDepthResources();
        bool CreateRenderCompleteSemaphores();
        void DestroyRenderCompleteSemaphores();

        Device *mDevice = nullptr;

        VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D mExtent{};

        std::vector<VkImage> mImages;
        std::vector<VkImageView> mViews;

        // One per swapchain image: present must wait on the semaphore of the image it shows.
        std::vector<VkSemaphore> mRenderComplete;

        VkImage mDepthImages[kFramesInFlight]{};
        VmaAllocation mDepthAllocations[kFramesInFlight]{};
        VkImageView mDepthViews[kFramesInFlight]{};

        bool mCanCapture = false;
        bool mOutOfDate = false;
    };
}
