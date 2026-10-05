/**
 * @file VulkanRendererImGui.cpp
 * @author Sumin Park
 * @brief ImGui Vulkan backend init and shutdown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>
#include "vulkan/Device.h"
#include "vulkan/Swapchain.h"
#include "vulkan/VulkanConstants.h"

#include <core/log/Log.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace mir
{
    namespace
    {
        void CheckImGuiVulkanResult(VkResult result)
        {
            if (result != VK_SUCCESS)
                MIR_LOG_ERROR("CheckImGuiVulkanResult: ImGui Vulkan backend call failed: {}", static_cast<int>(result));
        }
    }

    bool VulkanRenderer::InitImGuiVulkanBackend()
    {
        const VkFormat colorFormat = mSwapchain->Format();
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorFormat,
            .depthAttachmentFormat = vk::kDepthFormat };

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = vk::Device::kApiVersion;
        initInfo.Instance = mDevice->Instance();
        initInfo.PhysicalDevice = mDevice->Physical();
        initInfo.Device = mDevice->Handle();
        initInfo.QueueFamily = mDevice->QueueFamily();
        initInfo.Queue = mDevice->Queue();
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = static_cast<uint32_t>(mSwapchain->ImageCount());
        initInfo.ImageCount = static_cast<uint32_t>(mSwapchain->ImageCount());
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;

        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            MIR_LOG_ERROR("InitImGuiVulkanBackend: ImGui_ImplVulkan_Init failed");
            return false;
        }

        mImGuiBackendInitialized = true;
        return true;
    }

    void VulkanRenderer::ShutdownImGuiVulkanBackend()
    {
        if (!mImGuiBackendInitialized)
            return;

        vkDeviceWaitIdle(mDevice->Handle());
        ImGui_ImplVulkan_Shutdown();
        mImGuiBackendInitialized = false;
    }
}
