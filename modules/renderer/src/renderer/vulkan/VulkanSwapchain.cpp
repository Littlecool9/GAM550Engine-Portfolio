/**
 * @file VulkanSwapchain.cpp
 * @author Sumin Park
 * @brief Swapchain, its image views, depth images and render-complete semaphores.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>
#include <vector>

namespace mir
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

    bool VulkanRenderer::CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &caps);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(mPhysicalDevice, mSurface);
        const VkPresentModeKHR presentMode = ChoosePresentMode(mPhysicalDevice, mSurface);
        const VkExtent2D extent = ChooseExtent(caps, *mWindow);
        mCanCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainKHR oldSwapchain = mSwapchain;

        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = mSurface,
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
            .clipped = VK_TRUE,
            // reuse old resources
            .oldSwapchain = oldSwapchain };

        if (vkCreateSwapchainKHR(mDevice, &info, nullptr, &mSwapchain) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateSwapchainKHR failed");
            mSwapchain = VK_NULL_HANDLE;
            return false;
        }

        // Retired, not owned by the new swapchain: destroy it ourselves.
        if (oldSwapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(mDevice, oldSwapchain, nullptr);

        mSwapchainFormat = surfaceFormat.format;
        mSwapchainExtent = extent;

        // The swapchain creates and owns these. We never allocate or free them.
        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &actualCount, nullptr);
        mSwapchainImages.resize(actualCount);
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &actualCount, mSwapchainImages.data());

        MIR_LOG_INFO("Swapchain: {}x{} | {} image(s), requested {} | present mode {}",
            extent.width, extent.height, actualCount, imageCount,
            presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");

        return CreateImageViews() && CreateDepthResources() && CreateRtOutputImages();
    }
    bool VulkanRenderer::CreateImageViews()
    {
        mSwapchainViews.resize(mSwapchainImages.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < mSwapchainImages.size(); ++i)
        {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mSwapchainImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = mSwapchainFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice, &info, nullptr, &mSwapchainViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateImageView failed for swapchain image {}", i);
                return false;
            }
        }

        return true;
    }
    bool VulkanRenderer::CreateDepthResources()
    {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, kDepthFormat, &formatProps);
        if ((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        {
            MIR_LOG_CRITICAL("VK_FORMAT_D32_SFLOAT unsupported as an optimal-tiling depth attachment");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ mSwapchainExtent.width, mSwapchainExtent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
                &mDepthImages[i], &mDepthAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vmaCreateImage failed for depth buffer {}", i);
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

            if (vkCreateImageView(mDevice, &viewInfo, nullptr, &mDepthViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateImageView failed for depth buffer {}", i);
                return false;
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mDepthImages[i]),
                std::format("Depth buffer {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyDepthResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mDepthViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mDepthViews[i], nullptr);
                mDepthViews[i] = VK_NULL_HANDLE;
            }
            if (mDepthImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mDepthImages[i], mDepthAllocations[i]);
                mDepthImages[i] = VK_NULL_HANDLE;
                mDepthAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanRenderer::DestroySwapchain()
    {
        // swapchain and depth go hand in hand
        DestroyRtOutputImages();
        DestroyDepthResources();

        for (VkImageView view : mSwapchainViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, view, nullptr);
        }
        mSwapchainViews.clear();
        mSwapchainImages.clear();

        if (mSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        vkDeviceWaitIdle(mDevice);
        DestroyRenderCompleteSemaphores();
        DestroySwapchain();

        if (!CreateSwapchain())
            return false;
        if (!CreateRenderCompleteSemaphores())
            return false;

        if (mRayTracingSupported)
            WriteRtDescriptorSets();

        mNeedRecreate = false;
        return true;
    }

    bool VulkanRenderer::CreateRenderCompleteSemaphores()
    {
        const VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        mRenderComplete.resize(mSwapchainImages.size(), VK_NULL_HANDLE);

        for (VkSemaphore &sem : mRenderComplete)
        {
            if (vkCreateSemaphore(mDevice, &info, nullptr, &sem) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("render-complete semaphore creation failed");
                return false;
            }
        }
        return true;
    }

    void VulkanRenderer::DestroyRenderCompleteSemaphores()
    {
        for (VkSemaphore sem : mRenderComplete)
        {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(mDevice, sem, nullptr);
        }
        mRenderComplete.clear();
    }
}
