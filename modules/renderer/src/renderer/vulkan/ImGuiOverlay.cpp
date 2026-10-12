/**
 * @file ImGuiOverlay.cpp
 * @author Sumin Park
 * @brief ImGui Vulkan backend lifetime and overlay recording.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ImGuiOverlay.h"
#include "Device.h"
#include "Swapchain.h"
#include "VulkanConstants.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace mir::vk
{
    namespace
    {
        void CheckImGuiVulkanResult(VkResult result)
        {
            if (result != VK_SUCCESS)
                MIR_LOG_ERROR("CheckImGuiVulkanResult: ImGui Vulkan backend call failed: {}", static_cast<int>(result));
        }
    }

    bool ImGuiOverlay::Initialize(Device &device, const Swapchain &swapchain)
    {
        mDevice = &device;

        const VkFormat colorFormat = swapchain.Format();
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorFormat,
            .depthAttachmentFormat = kDepthFormat };

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = Device::kApiVersion;
        initInfo.Instance = mDevice->Instance();
        initInfo.PhysicalDevice = mDevice->Physical();
        initInfo.Device = mDevice->Handle();
        initInfo.QueueFamily = mDevice->QueueFamily();
        initInfo.Queue = mDevice->Queue();
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = swapchain.ImageCount();
        initInfo.ImageCount = swapchain.ImageCount();
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;

        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            MIR_LOG_ERROR("Initialize: ImGui_ImplVulkan_Init failed");
            return false;
        }

        mBackendInitialized = true;
        return true;
    }

    void ImGuiOverlay::Shutdown()
    {
        if (!mBackendInitialized)
            return;

        mDevice->WaitIdle();
        ImGui_ImplVulkan_Shutdown();
        mBackendInitialized = false;
        mDrawData = nullptr;
    }

    void ImGuiOverlay::RecordInPass(VkCommandBuffer cmd) const
    {
        if (mDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mDrawData, cmd);
    }

    VkImageLayout ImGuiOverlay::RecordOver(VkCommandBuffer cmd, const Swapchain &swapchain, uint32_t imageIndex,
        VkImageLayout layout) const
    {
        if (mDrawData == nullptr)
            return layout;

        // Only a transfer write can precede this on the ray traced path.
        ImageBarrier(cmd, swapchain.Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = swapchain.View(imageIndex),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE };

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{ 0, 0 }, .extent = swapchain.Extent() },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment };

        vkCmdBeginRendering(cmd, &renderingInfo);
        ImGui_ImplVulkan_RenderDrawData(mDrawData, cmd);
        vkCmdEndRendering(cmd);

        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
}
