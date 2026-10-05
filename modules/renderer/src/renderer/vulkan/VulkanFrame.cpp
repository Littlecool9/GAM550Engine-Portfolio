/**
 * @file VulkanFrame.cpp
 * @author Sumin Park
 * @brief Per-frame command recording and submission for the raster path.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <renderer/VulkanRenderer.h>
#include "FrameSync.h"
#include "VulkanDevice.h"
#include "VulkanSwapchain.h"
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <core/log/Log.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>

#include <algorithm>

namespace mir
{
    void VulkanRenderer::RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex)
    {
        const VkRect2D rect = vk::ResolveViewRect(view.rect, mSwapchain->Extent());

        const VkViewport viewport{
            .x = static_cast<float>(rect.offset.x),
            .y = static_cast<float>(rect.offset.y),
            .width = static_cast<float>(std::max(rect.extent.width, 1u)),
            .height = static_cast<float>(std::max(rect.extent.height, 1u)),
            .minDepth = 0.0f,
            .maxDepth = 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);

        WriteCameraData(view, viewIndex);
        const uint32_t cameraOffset = static_cast<uint32_t>(viewIndex * mCameraAlignedSize);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
            1, 1, &mCameraDescriptorSets[mFrameSync->Index()], 1, &cameraOffset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
            3, 1, &mShadowDescriptorSets[mFrameSync->Index()], 0, nullptr);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundDescriptorSet = VK_NULL_HANDLE;

        // no mesh/material batching yet, just draw per item
        for (const DrawItem &item : view.items)
        {
            const GpuMesh *const mesh = FindMesh(item.mesh);
            if (mesh == nullptr)
                continue;

            const GpuMaterial *material = FindMaterial(item.material);
            // A slot emptied by ReleaseModelResources is still in range, so
            // check the pipeline rather than the handle alone.
            if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
                material = FindMaterial(mDefaultMaterial);
            if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
                continue;

            if (material->mModelShader && !BindModelDescriptors(cmd, *material, item))
                continue;

            if (material->mPipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->mPipeline);
                boundPipeline = material->mPipeline;
            }

            if (material->mDescriptorSet != boundDescriptorSet)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
                    0, 1, &material->mDescriptorSet, 0, nullptr);
                boundDescriptorSet = material->mDescriptorSet;
            }

            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);

            vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

            const vk::RasterPushConstants pushData{
                .model = item.model,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .tint = item.tint };

            vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(vk::RasterPushConstants), &pushData);

            vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
        }

        // Sky last, at the far depth 0: the test (GEQUAL, no write) passes only
        // where nothing above wrote depth, i.e. still at the clear value - a
        // fullscreen ray-marched shader paying its cost only on actual
        // background pixels, with early-z doing the rejection.
        if (mSkyVisible)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSkyPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSkyPipelineLayout,
                0, 1, &mCameraDescriptorSets[mFrameSync->Index()], 1, &cameraOffset);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }

    void VulkanRenderer::RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        // get image ready
        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        vk::ImageBarrier(cmd, mSwapchain->DepthImage(mFrameSync->Index()), VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->View(imageIndex),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .color{ { mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } } } };

        const VkRenderingAttachmentInfo depthAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->DepthView(mFrameSync->Index()),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue{ .depthStencil{ .depth = 0.0f } } }; // reverse-Z: 0 is the far end

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            // The stored extent, never the window: re-deriving it invites drift.
            .renderArea{ .offset{ 0, 0 }, .extent = mSwapchain->Extent() },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
            .pDepthAttachment = &depthAttachment };

        vkCmdBeginRendering(cmd, &renderingInfo);

        if (mDevice->ValidationEnabled())
        {
            const VkDebugUtilsLabelEXT label{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = "Triangle Pass",
                .color{ 1.0f, 0.0f, 1.0f, 1.0f } };
            vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
        }

        if (views.size() > kMaxViewsPerFrame)
        {
            MIR_LOG_ERROR("RecordCommands: {} views this frame, only the first {} (kMaxViewsPerFrame) are drawn",
                views.size(), kMaxViewsPerFrame);
        }

        const size_t viewCount = std::min<size_t>(views.size(), kMaxViewsPerFrame);
        for (size_t i = 0; i < viewCount; ++i)
            RecordView(cmd, views[i], static_cast<uint32_t>(i));

        if (mImguiDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mImguiDrawData, cmd);

        if (mDevice->ValidationEnabled())
            vkCmdEndDebugUtilsLabelEXT(cmd);

        vkCmdEndRendering(cmd);

        const bool captured = RecordCapture(cmd, imageIndex);

        // give it back
        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            captured ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            captured ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0);
    }

    std::optional<vk::FrameContext> VulkanRenderer::BeginFrame()
    {
        if (mWindow->Width() == 0 || mWindow->Height() == 0)
            return std::nullopt;

        if (mSwapchain->NeedsRecreate(*mWindow) && !RecreateSwapchain())
            return std::nullopt;

        const std::optional<vk::FrameContext> frame = mFrameSync->Begin(*mSwapchain);
        // After Begin's wait: this slot's last frame is done with its set 2 buffers.
        if (frame)
            ResetModelFrame(mFrameSync->Index());
        return frame;
    }

    void VulkanRenderer::DrawFrame(std::span<const RenderView> views)
    {
        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        RecordShadowPasses(frame->cmd);
        RecordCommands(frame->cmd, frame->imageIndex, views);
        mFrameSync->End(*mSwapchain, *frame);

        // Waits for the queue to go idle, so the capture is complete.
        if (mCaptureBuffer != VK_NULL_HANDLE)
            FinishCapture();
    }
}
