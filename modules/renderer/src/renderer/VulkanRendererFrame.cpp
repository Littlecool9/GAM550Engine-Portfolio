/**
 * @file VulkanRendererFrame.cpp
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
#include "vulkan/CameraBuffer.h"
#include "vulkan/Device.h"
#include "vulkan/FrameSync.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/RayTracer.h"
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/Swapchain.h"
#include "vulkan/VulkanUtil.h"
#include "vulkan/VulkanPushConstants.h"

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

        const uint32_t cameraOffset = WriteCameraData(view, viewIndex);
        const VkDescriptorSet cameraSet = mCameraBuffer->Set(mFrameSync->Index());
        const VkDescriptorSet shadowSet = mShadowMaps->Set(mFrameSync->Index());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            1, 1, &cameraSet, 1, &cameraOffset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            3, 1, &shadowSet, 0, nullptr);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundDescriptorSet = VK_NULL_HANDLE;

        // no mesh/material batching yet, just draw per item
        for (const DrawItem &item : view.items)
        {
            const vk::GpuMesh *const mesh = mResources->FindMesh(item.mesh);
            if (mesh == nullptr)
                continue;

            const vk::GpuMaterial *const material = mMaterialStore->FindOrDefault(item.material);
            if (material == nullptr)
                continue;

            if (material->mModelShader &&
                !mModelDescriptors->Bind(cmd, mMaterialStore->PipelineLayout(), mFrameSync->Index(),
                    item.deformation, material->mParameters))
                continue;

            if (material->mPipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->mPipeline);
                boundPipeline = material->mPipeline;
            }

            if (material->mDescriptorSet != boundDescriptorSet)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
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

            vkCmdPushConstants(cmd, mMaterialStore->PipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT,
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
                0, 1, &cameraSet, 1, &cameraOffset);
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

        if (views.size() > vk::kMaxViewsPerFrame)
        {
            MIR_LOG_ERROR("RecordCommands: {} views this frame, only the first {} (kMaxViewsPerFrame) are drawn",
                views.size(), vk::kMaxViewsPerFrame);
        }

        const size_t viewCount = std::min<size_t>(views.size(), vk::kMaxViewsPerFrame);
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
        if (mFrameSync->Stopped() || mWindow->Width() == 0 || mWindow->Height() == 0)
            return std::nullopt;

        if (mSwapchain->NeedsRecreate(*mWindow) && !RecreateSwapchain())
            return std::nullopt;

        const std::optional<vk::FrameContext> frame = mFrameSync->Begin(*mSwapchain);
        // After Begin's wait: this slot's last frame is done with its set 2 buffers.
        if (frame)
            mModelDescriptors->ResetFrame(mFrameSync->Index());
        return frame;
    }

    void VulkanRenderer::DrawFrame(std::span<const RenderView> views)
    {
        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        mShadowMaps->RecordPasses(frame->cmd, mFrameSync->Index(), *mResources);
        RecordCommands(frame->cmd, frame->imageIndex, views);
        const bool submitted = mFrameSync->End(*mSwapchain, *frame);

        if (mCaptureBuffer != VK_NULL_HANDLE)
            FinishCapture(submitted);
    }

    void VulkanRenderer::RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        const RenderView &view = views[0];

        const VkRect2D rect = vk::ResolveViewRect(view.rect, mSwapchain->Extent());

        const uint32_t cameraOffset = WriteCameraData(view, 0);

        if (mDevice->ValidationEnabled())
        {
            const VkDebugUtilsLabelEXT label{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = "Ray Trace Pass",
                .color{ 0.0f, 1.0f, 1.0f, 1.0f } };
            vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
        }

        mRayTracer->Record(cmd, mFrameSync->Index(), view.items, rect, mCameraBuffer->Set(mFrameSync->Index()),
            cameraOffset, *mResources, *mMaterialStore);

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkClearColorValue clearColor{ .float32{ mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } };
        const VkImageSubresourceRange wholeImage{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
        vkCmdClearColorImage(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor, 1, &wholeImage);

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        if (rect.extent.width > 0 && rect.extent.height > 0)
        {
            const int32_t left = rect.offset.x;
            const int32_t top = rect.offset.y;
            const int32_t right = left + static_cast<int32_t>(rect.extent.width);
            const int32_t bottom = top + static_cast<int32_t>(rect.extent.height);
            const VkImageBlit blitRegion{
                .srcSubresource{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
                .srcOffsets{ { left, top, 0 },{ right, bottom, 1 } },
                .dstSubresource{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
                .dstOffsets{ { left, top, 0 },{ right, bottom, 1 } } };

            vkCmdBlitImage(cmd,
                mRayTracer->OutputImage(mFrameSync->Index()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                mSwapchain->Image(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blitRegion, VK_FILTER_NEAREST);
        }

        if (mImguiDrawData != nullptr)
        {
            vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            const VkRenderingAttachmentInfo colorAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mSwapchain->View(imageIndex),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE };

            const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent = mSwapchain->Extent() },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &colorAttachment };

            vkCmdBeginRendering(cmd, &renderingInfo);
            ImGui_ImplVulkan_RenderDrawData(mImguiDrawData, cmd);
            vkCmdEndRendering(cmd);

            vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
        }
        else
        {
            vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
        }

        if (mDevice->ValidationEnabled())
            vkCmdEndDebugUtilsLabelEXT(cmd);
    }

    void VulkanRenderer::TraceFrame(std::span<const RenderView> views)
    {
        if (!mDevice->RayTracingSupported())
        {
            MIR_LOG_ERROR("TraceFrame: called without ray tracing support; ignored");
            return;
        }

        if (views.empty())
            return;

        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        RecordRtFrame(frame->cmd, frame->imageIndex, views);
        mFrameSync->End(*mSwapchain, *frame);
    }

    uint32_t VulkanRenderer::WriteCameraData(const RenderView &view, uint32_t viewIndex)
    {
        const uint32_t frame = mFrameSync->Index();
        const vk::ViewShadowInfo shadow = mShadowMaps->WriteView(frame, view, viewIndex);
        return mCameraBuffer->WriteView(frame, view, viewIndex, Lighting(), shadow);
    }

    vk::SceneLighting VulkanRenderer::Lighting() const
    {
        return vk::SceneLighting{
            .lightDirection = mLightDirection,
            .lightColor = mLightColor,
            .ambientColor = mAmbientColor,
            .fogColor = mFogColor,
            .fogDensity = mFogDensity,
            .atmosphere = mAtmosphere };
    }
}
