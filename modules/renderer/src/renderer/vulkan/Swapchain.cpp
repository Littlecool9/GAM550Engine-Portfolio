/**
 * @file Swapchain.cpp
 * @author Sumin Park
 * @brief Swapchain, its image views, depth images and render-complete semaphores.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "Swapchain.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/platform/Surface.h>

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>
#include <vector>

namespace mir::vk
{
    namespace
    {
        VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());

            for (const VkSurfaceFormatKHR &format : formats)
            {
                if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    return format;
            }

            // fall back to basic format. It'll have the basics
            return formats[0];
        }

        VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());

            for (VkPresentModeKHR mode : modes)
            {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                    return mode;
            }

            // FIFO is the only mode the spec guarantees exists.
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR &caps, const ISurfaceProvider &window)
        {
            if (caps.currentExtent.width != UINT32_MAX)
                return caps.currentExtent;

            // Framebuffer size, not window size: they differ on HiDPI.
            VkExtent2D extent{ window.Width(), window.Height() };

            extent.width = std::clamp(extent.width,
                caps.minImageExtent.width,
                caps.maxImageExtent.width);
            extent.height = std::clamp(extent.height,
                caps.minImageExtent.height,
                caps.maxImageExtent.height);
            return extent;
        }
    }

    bool Swapchain::Initialize(Device &device, const ISurfaceProvider &window)
    {
        mDevice = &device;
        return Create(window);
    }

    void Swapchain::Shutdown()
    {
        if (mDevice != nullptr)
            Destroy();
    }

    bool Swapchain::Recreate(const ISurfaceProvider &window)
    {
        Destroy();
        if (!Create(window))
            return false;

        mOutOfDate = false;
        return true;
    }

    bool Swapchain::NeedsRecreate(const ISurfaceProvider &window) const
    {
        return mOutOfDate || window.Width() != mExtent.width || window.Height() != mExtent.height;
    }

    bool Swapchain::Acquire(VkSemaphore signal, uint32_t &outImageIndex)
    {
        const VkResult result = vkAcquireNextImageKHR(
            mDevice->Handle(), mSwapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &outImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            mOutOfDate = true;
            return false;
        }

        // Signalled and usable. Finish the frame, recreate next one.
        if (result == VK_SUBOPTIMAL_KHR)
        {
            mOutOfDate = true;
        }
        else if (result != VK_SUCCESS)
        {
            MIR_LOG_ERROR("Acquire: vkAcquireNextImageKHR failed: {}", static_cast<int>(result));
            return false;
        }

        return true;
    }

    void Swapchain::Present(uint32_t imageIndex)
    {
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &mRenderComplete[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &mSwapchain,
            .pImageIndices = &imageIndex };

        const VkResult result = vkQueuePresentKHR(mDevice->Queue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            mOutOfDate = true;
        else if (result != VK_SUCCESS)
            MIR_LOG_ERROR("Present: vkQueuePresentKHR failed: {}", static_cast<int>(result));
    }

    void Swapchain::RecordPresentTransition(VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout from) const
    {
        VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkAccessFlags2 srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        if (from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        else if (from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }

        ImageBarrier(cmd, mImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            from, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, srcStage, srcAccess, VK_PIPELINE_STAGE_2_NONE, 0);
    }

    bool Swapchain::Create(const ISurfaceProvider &window)
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice->Physical(), mDevice->Surface(), &caps);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(mDevice->Physical(), mDevice->Surface());
        const VkPresentModeKHR presentMode = ChoosePresentMode(mDevice->Physical(), mDevice->Surface());
        const VkExtent2D extent = ChooseExtent(caps, window);
        mCanCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = mDevice->Surface(),
            .minImageCount = imageCount,
            .imageFormat = surfaceFormat.format,
            .imageColorSpace = surfaceFormat.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (mCanCapture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
            // One family does graphics and present, so no ownership transfers.
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = presentMode,
            // Allow the driver to skip shading pixels hidden by other windows.
            .clipped = VK_TRUE };

        if (vkCreateSwapchainKHR(mDevice->Handle(), &info, nullptr, &mSwapchain) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Create: vkCreateSwapchainKHR failed");
            mSwapchain = VK_NULL_HANDLE;
            return false;
        }

        mFormat = surfaceFormat.format;
        mExtent = extent;

        // The swapchain creates and owns these. We never allocate or free them.
        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(mDevice->Handle(), mSwapchain, &actualCount, nullptr);
        mImages.resize(actualCount);
        vkGetSwapchainImagesKHR(mDevice->Handle(), mSwapchain, &actualCount, mImages.data());

        MIR_LOG_INFO("Swapchain: {}x{} | {} image(s), requested {} | present mode {}",
            extent.width, extent.height, actualCount, imageCount,
            presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");

        if (!CreateImageViews() || !CreateDepthResources() || !CreateRenderCompleteSemaphores())
            return false;

        mDevice->NameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mSwapchain), "Swapchain");
        for (size_t i = 0; i < mImages.size(); ++i)
        {
            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mImages[i]),
                std::format("Swapchain image {}", i).c_str());
        }

        return true;
    }

    bool Swapchain::CreateImageViews()
    {
        mViews.resize(mImages.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < mImages.size(); ++i)
        {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = mFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice->Handle(), &info, nullptr, &mViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateImageViews: vkCreateImageView failed for swapchain image {}", i);
                return false;
            }
        }

        return true;
    }
    bool Swapchain::CreateDepthResources()
    {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(mDevice->Physical(), kDepthFormat, &formatProps);
        if ((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        {
            MIR_LOG_CRITICAL("CreateDepthResources: VK_FORMAT_D32_SFLOAT unsupported as an optimal-tiling depth attachment");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ mExtent.width, mExtent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
                &mDepthImages[i], &mDepthAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateDepthResources: vmaCreateImage failed for depth buffer {}", i);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mDepthImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kDepthFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &mDepthViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateDepthResources: vkCreateImageView failed for depth buffer {}", i);
                return false;
            }

            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mDepthImages[i]),
                std::format("Depth buffer {}", i).c_str());
        }

        return true;
    }

    void Swapchain::DestroyDepthResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mDepthViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice->Handle(), mDepthViews[i], nullptr);
                mDepthViews[i] = VK_NULL_HANDLE;
            }
            if (mDepthImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mDevice->Allocator(), mDepthImages[i], mDepthAllocations[i]);
                mDepthImages[i] = VK_NULL_HANDLE;
                mDepthAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    void Swapchain::Destroy()
    {
        DestroyRenderCompleteSemaphores();
        // swapchain and depth go hand in hand
        DestroyDepthResources();

        for (VkImageView view : mViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice->Handle(), view, nullptr);
        }
        mViews.clear();
        mImages.clear();

        if (mSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(mDevice->Handle(), mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
    }

    bool Swapchain::CreateRenderCompleteSemaphores()
    {
        const VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        mRenderComplete.resize(mImages.size(), VK_NULL_HANDLE);

        for (VkSemaphore &sem : mRenderComplete)
        {
            if (vkCreateSemaphore(mDevice->Handle(), &info, nullptr, &sem) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateRenderCompleteSemaphores: render-complete semaphore creation failed");
                return false;
            }
        }
        return true;
    }

    void Swapchain::DestroyRenderCompleteSemaphores()
    {
        for (VkSemaphore sem : mRenderComplete)
        {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(mDevice->Handle(), sem, nullptr);
        }
        mRenderComplete.clear();
    }
}
