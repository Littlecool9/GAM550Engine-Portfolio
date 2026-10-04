/**
 * @file VulkanImGui.cpp
 * @author Sumin Park
 * @brief ImGui Vulkan backend init and shutdown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>

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
                MIR_LOG_ERROR("ImGui Vulkan backend call failed: {}", static_cast<int>(result));
        }
    }

    bool VulkanRenderer::InitImGuiVulkanBackend()
    {
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &mSwapchainFormat,
            .depthAttachmentFormat = kDepthFormat };

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VulkanVersion;
        initInfo.Instance = mVulkanInstance;
        initInfo.PhysicalDevice = mPhysicalDevice;
        initInfo.Device = mDevice;
        initInfo.QueueFamily = mGfxQueueFamIdx;
        initInfo.Queue = mGfxQueue;
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = static_cast<uint32_t>(mSwapchainImages.size());
        initInfo.ImageCount = static_cast<uint32_t>(mSwapchainImages.size());
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;

        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            MIR_LOG_ERROR("ImGui_ImplVulkan_Init failed");
            return false;
        }

        mImGuiBackendInitialized = true;
        return true;
    }

    void VulkanRenderer::ShutdownImGuiVulkanBackend()
    {
        if (!mImGuiBackendInitialized)
            return;

        vkDeviceWaitIdle(mDevice);
        ImGui_ImplVulkan_Shutdown();
        mImGuiBackendInitialized = false;
    }
}
