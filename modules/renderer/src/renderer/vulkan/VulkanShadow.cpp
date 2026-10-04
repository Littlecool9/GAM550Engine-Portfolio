/**
 * @file VulkanShadow.cpp
 * @author Sumin Park
 * @brief Sun, spot and point shadow maps: resources, casters and depth passes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>

namespace mir
{
 
    bool VulkanRenderer::CreateShadowResources()
    {
        const VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .compareEnable = VK_FALSE,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };

        if (vkCreateSampler(mDevice, &samplerInfo, nullptr, &mShadowSampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreateSampler failed");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ kSunShadowResolution, kSunShadowResolution, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
                &mSunShadowImages[i], &mSunShadowAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vmaCreateImage failed for Sun shadow map {}", i);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mSunShadowImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kDepthFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice, &viewInfo, nullptr, &mSunShadowViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Sun shadow map {}", i);
                return false;
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mSunShadowImages[i]),
                std::format("Sun shadow map {}", i).c_str());
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ kSpotShadowResolution, kSpotShadowResolution, 1 },
                .mipLevels = 1,
                .arrayLayers = kMaxSpotShadows,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
                &mSpotShadowImages[i], &mSpotShadowAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vmaCreateImage failed for Spot shadow array {}", i);
                return false;
            }

            const VkImageViewCreateInfo arrayViewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mSpotShadowImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format = kDepthFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .levelCount = 1,
                    .layerCount = kMaxSpotShadows } };

            if (vkCreateImageView(mDevice, &arrayViewInfo, nullptr, &mSpotShadowArrayViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Spot shadow array {}", i);
                return false;
            }

            for (uint32_t slot = 0; slot < kMaxSpotShadows; ++slot)
            {
                const VkImageViewCreateInfo layerViewInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = mSpotShadowImages[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = kDepthFormat,
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                        .levelCount = 1,
                        .baseArrayLayer = slot,
                        .layerCount = 1 } };

                if (vkCreateImageView(mDevice, &layerViewInfo, nullptr, &mSpotShadowLayerViews[i][slot]) != VK_SUCCESS)
                {
                    MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Spot shadow slot {}/{}",
                        i, slot);
                    return false;
                }
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mSpotShadowImages[i]),
                std::format("Spot shadow array {}", i).c_str());

            const VkDeviceSize spotMatrixBufferSize = sizeof(glm::mat4) * kMaxSpotShadows * kMaxViewsPerFrame;
            VmaAllocationInfo spotMatrixAllocInfo{};
            if (!vk::CreateBuffer(mAllocator, spotMatrixBufferSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mSpotShadowMatrixBuffers[i], mSpotShadowMatrixAllocations[i], &spotMatrixAllocInfo))
            {
                MIR_LOG_CRITICAL("CreateShadowResources: Spot shadow matrix buffer allocation failed for frame {}", i);
                return false;
            }
            mSpotShadowMatrixMapped[i] = static_cast<std::byte *>(spotMatrixAllocInfo.pMappedData);
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            constexpr uint32_t kPointLayers = kMaxPointShadows * 6;

            const VkImageCreateInfo depthImageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ kPointShadowResolution, kPointShadowResolution, 1 },
                .mipLevels = 1,
                .arrayLayers = kPointLayers,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &depthImageInfo, &allocInfo,
                &mPointShadowDepthImages[i], &mPointShadowDepthAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vmaCreateImage failed for Point shadow depth {}", i);
                return false;
            }

            const VkImageCreateInfo colorImageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R32_SFLOAT,
                .extent{ kPointShadowResolution, kPointShadowResolution, 1 },
                .mipLevels = 1,
                .arrayLayers = kPointLayers,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            if (vmaCreateImage(mAllocator, &colorImageInfo, &allocInfo,
                &mPointShadowColorImages[i], &mPointShadowColorAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vmaCreateImage failed for Point shadow distance {}", i);
                return false;
            }

            for (uint32_t layer = 0; layer < kPointLayers; ++layer)
            {
                const VkImageViewCreateInfo depthLayerViewInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = mPointShadowDepthImages[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = kDepthFormat,
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                        .levelCount = 1,
                        .baseArrayLayer = layer,
                        .layerCount = 1 } };

                if (vkCreateImageView(mDevice, &depthLayerViewInfo, nullptr,
                    &mPointShadowDepthLayerViews[i][layer]) != VK_SUCCESS)
                {
                    MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Point depth {}/{}",
                        i, layer);
                    return false;
                }

                const VkImageViewCreateInfo colorLayerViewInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = mPointShadowColorImages[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = VK_FORMAT_R32_SFLOAT,
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1,
                        .baseArrayLayer = layer,
                        .layerCount = 1 } };

                if (vkCreateImageView(mDevice, &colorLayerViewInfo, nullptr,
                    &mPointShadowColorLayerViews[i][layer]) != VK_SUCCESS)
                {
                    MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Point color {}/{}",
                        i, layer);
                    return false;
                }
            }

            const VkImageViewCreateInfo cubeArrayViewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mPointShadowColorImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
                .format = VK_FORMAT_R32_SFLOAT,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = kPointLayers } };

            if (vkCreateImageView(mDevice, &cubeArrayViewInfo, nullptr, &mPointShadowCubeArrayViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: vkCreateImageView failed for Point cube array {}", i);
                return false;
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mPointShadowColorImages[i]),
                std::format("Point shadow distance cube array {}", i).c_str());
        }

        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{ {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } } };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice, &setLayoutInfo, nullptr, &mShadowSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const std::array<VkDescriptorPoolSize, 2> poolSizes{ {
            { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kFramesInFlight * 3 },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = kFramesInFlight } } };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data() };

        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mShadowDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreateDescriptorPool failed");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mShadowDescriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mShadowSetLayout };

            if (vkAllocateDescriptorSets(mDevice, &setAllocInfo, &mShadowDescriptorSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateShadowResources: descriptor set allocation failed for frame {}", i);
                return false;
            }

            const VkDescriptorImageInfo sunImageInfo{
                .sampler = mShadowSampler,
                .imageView = mSunShadowViews[i],
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

            const VkDescriptorImageInfo spotImageInfo{
                .sampler = mShadowSampler,
                .imageView = mSpotShadowArrayViews[i],
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

            const VkDescriptorBufferInfo spotMatrixBufferInfo{
                .buffer = mSpotShadowMatrixBuffers[i],
                .offset = 0,
                .range = sizeof(glm::mat4) * kMaxSpotShadows * kMaxViewsPerFrame };

            const VkDescriptorImageInfo pointImageInfo{
                .sampler = mShadowSampler,
                .imageView = mPointShadowCubeArrayViews[i],
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            const std::array<VkWriteDescriptorSet, 4> writes{ {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mShadowDescriptorSets[i],
                 .dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sunImageInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mShadowDescriptorSets[i],
                 .dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &spotImageInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mShadowDescriptorSets[i],
                 .dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &spotMatrixBufferInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mShadowDescriptorSets[i],
                 .dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &pointImageInfo } } };

            vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        VkShaderModule vertModule = vk::CreateShaderModule(mDevice, ShaderPath("shadowDepth.vertexMain.spv"));
        if (vertModule == VK_NULL_HANDLE)
            return false;

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(vk::ShadowPushConstants) };

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice, &layoutInfo, nullptr, &mShadowDepthPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreatePipelineLayout failed");
            vkDestroyShaderModule(mDevice, vertModule, nullptr);
            return false;
        }

        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertModule,
            .pName = "vertexMain" };

        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const VkVertexInputAttributeDescription positionAttribute{
            .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &positionAttribute };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_TRUE,
            .depthBiasConstantFactor = 1.25f,
            .depthBiasSlopeFactor = 1.75f,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE };

        const VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 0,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 1,
            .pStages = &stage,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depthStencil,
            .pDynamicState = &dynamicState,
            .layout = mShadowDepthPipelineLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        const VkResult pipelineResult = vkCreateGraphicsPipelines(
            mDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mShadowDepthPipeline);

        vkDestroyShaderModule(mDevice, vertModule, nullptr);

        if (pipelineResult != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreateGraphicsPipelines failed: {}",
                static_cast<int>(pipelineResult));
            return false;
        }

        VkShaderModule distanceVertModule = vk::CreateShaderModule(mDevice, ShaderPath("shadowDistance.vertexMain.spv"));
        VkShaderModule distanceFragModule = vk::CreateShaderModule(mDevice, ShaderPath("shadowDistance.fragmentMain.spv"));
        if (distanceVertModule == VK_NULL_HANDLE || distanceFragModule == VK_NULL_HANDLE)
        {
            if (distanceVertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, distanceVertModule, nullptr);
            if (distanceFragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, distanceFragModule, nullptr);
            return false;
        }

        const std::array<VkPipelineShaderStageCreateInfo, 2> distanceStages{ {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = distanceVertModule, .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = distanceFragModule, .pName = "fragmentMain" } } };

        const VkPipelineColorBlendAttachmentState distanceBlendAttachment{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT };

        const VkPipelineColorBlendStateCreateInfo distanceColorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &distanceBlendAttachment };

        constexpr VkFormat kPointShadowColorFormat = VK_FORMAT_R32_SFLOAT;
        const VkPipelineRenderingCreateInfo distanceRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &kPointShadowColorFormat,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo distancePipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &distanceRenderingInfo,
            .stageCount = static_cast<uint32_t>(distanceStages.size()),
            .pStages = distanceStages.data(),
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depthStencil,
            .pColorBlendState = &distanceColorBlend,
            .pDynamicState = &dynamicState,
            .layout = mShadowDepthPipelineLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        const VkResult distancePipelineResult = vkCreateGraphicsPipelines(
            mDevice, VK_NULL_HANDLE, 1, &distancePipelineInfo, nullptr, &mPointShadowPipeline);

        vkDestroyShaderModule(mDevice, distanceVertModule, nullptr);
        vkDestroyShaderModule(mDevice, distanceFragModule, nullptr);

        if (distancePipelineResult != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShadowResources: vkCreateGraphicsPipelines (Point) failed: {}",
                static_cast<int>(distancePipelineResult));
            return false;
        }

        return true;
    }

    void VulkanRenderer::DestroyShadowResources()
    {
        if (mShadowDepthPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice, mShadowDepthPipeline, nullptr);
            mShadowDepthPipeline = VK_NULL_HANDLE;
        }
        if (mPointShadowPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice, mPointShadowPipeline, nullptr);
            mPointShadowPipeline = VK_NULL_HANDLE;
        }
        if (mShadowDepthPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice, mShadowDepthPipelineLayout, nullptr);
            mShadowDepthPipelineLayout = VK_NULL_HANDLE;
        }
        if (mShadowDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice, mShadowDescriptorPool, nullptr);
            mShadowDescriptorPool = VK_NULL_HANDLE;
        }
        if (mShadowSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice, mShadowSetLayout, nullptr);
            mShadowSetLayout = VK_NULL_HANDLE;
        }
        if (mShadowSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice, mShadowSampler, nullptr);
            mShadowSampler = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mSunShadowViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mSunShadowViews[i], nullptr);
                mSunShadowViews[i] = VK_NULL_HANDLE;
            }
            if (mSunShadowImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mSunShadowImages[i], mSunShadowAllocations[i]);
                mSunShadowImages[i] = VK_NULL_HANDLE;
                mSunShadowAllocations[i] = VK_NULL_HANDLE;
            }

            if (mSpotShadowMatrixBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mSpotShadowMatrixBuffers[i], mSpotShadowMatrixAllocations[i]);
                mSpotShadowMatrixBuffers[i] = VK_NULL_HANDLE;
                mSpotShadowMatrixAllocations[i] = VK_NULL_HANDLE;
                mSpotShadowMatrixMapped[i] = nullptr;
            }

            if (mSpotShadowArrayViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mSpotShadowArrayViews[i], nullptr);
                mSpotShadowArrayViews[i] = VK_NULL_HANDLE;
            }
            for (uint32_t slot = 0; slot < kMaxSpotShadows; ++slot)
            {
                if (mSpotShadowLayerViews[i][slot] != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(mDevice, mSpotShadowLayerViews[i][slot], nullptr);
                    mSpotShadowLayerViews[i][slot] = VK_NULL_HANDLE;
                }
            }
            if (mSpotShadowImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mSpotShadowImages[i], mSpotShadowAllocations[i]);
                mSpotShadowImages[i] = VK_NULL_HANDLE;
                mSpotShadowAllocations[i] = VK_NULL_HANDLE;
            }

            if (mPointShadowCubeArrayViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mPointShadowCubeArrayViews[i], nullptr);
                mPointShadowCubeArrayViews[i] = VK_NULL_HANDLE;
            }
            for (uint32_t layer = 0; layer < kMaxPointShadows * 6; ++layer)
            {
                if (mPointShadowDepthLayerViews[i][layer] != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(mDevice, mPointShadowDepthLayerViews[i][layer], nullptr);
                    mPointShadowDepthLayerViews[i][layer] = VK_NULL_HANDLE;
                }
                if (mPointShadowColorLayerViews[i][layer] != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(mDevice, mPointShadowColorLayerViews[i][layer], nullptr);
                    mPointShadowColorLayerViews[i][layer] = VK_NULL_HANDLE;
                }
            }
            if (mPointShadowDepthImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mPointShadowDepthImages[i], mPointShadowDepthAllocations[i]);
                mPointShadowDepthImages[i] = VK_NULL_HANDLE;
                mPointShadowDepthAllocations[i] = VK_NULL_HANDLE;
            }
            if (mPointShadowColorImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mPointShadowColorImages[i], mPointShadowColorAllocations[i]);
                mPointShadowColorImages[i] = VK_NULL_HANDLE;
                mPointShadowColorAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanRenderer::SetSunShadowCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor,
        std::span<const DrawItem> items)
    {
        mHasSunShadow = active;
        mSunShadowViewProj = viewProj;
        mSunShadowAnchor = anchor;
        mSunShadowItems = items;
    }

    void VulkanRenderer::SetSpotShadowCaster(uint32_t slot, bool active, const glm::mat4 &viewProj,
        WorldPosition anchor, std::span<const DrawItem> items)
    {
        if (slot >= kMaxSpotShadows)
            return;
        mSpotShadowCasters[slot] = SpotShadowCaster{ active, viewProj, anchor, items };
    }

    void VulkanRenderer::SetPointShadowCaster(uint32_t slot, bool active, WorldPosition position, float range,
        const std::array<PointShadowFace, 6> &faces)
    {
        if (slot >= kMaxPointShadows)
            return;
        mPointShadowCasters[slot] = PointShadowCaster{ active, position, range, faces };
    }

    void VulkanRenderer::RecordShadowPasses(VkCommandBuffer cmd)
    {
        const auto drawItems = [&](const glm::mat4 &viewProj, std::span<const DrawItem> items)
            {
                for (const DrawItem &item : items)
                {
                    const GpuMesh *const mesh = FindMesh(item.mesh);
                    if (mesh == nullptr)
                        continue;

                    const VkDeviceSize vertexOffset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);
                    vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

                    const vk::ShadowPushConstants pushData{ .viewProj = viewProj, .model = item.model };
                    vkCmdPushConstants(cmd, mShadowDepthPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                        0, sizeof(vk::ShadowPushConstants), &pushData);

                    vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
                }
            };

        if (mHasSunShadow)
        {
            vk::ImageBarrier(cmd, mSunShadowImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

            const VkRenderingAttachmentInfo depthAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mSunShadowViews[mFrameIndex],
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue{ .depthStencil{ .depth = 1.0f } } }; // forward Z here: 1 is the far end

            const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent{ kSunShadowResolution, kSunShadowResolution } },
                .layerCount = 1,
                .pDepthAttachment = &depthAttachment };

            vkCmdBeginRendering(cmd, &renderingInfo);

            const VkViewport viewport{ .x = 0.0f, .y = 0.0f,
                .width = static_cast<float>(kSunShadowResolution), .height = static_cast<float>(kSunShadowResolution),
                .minDepth = 0.0f, .maxDepth = 1.0f };
            const VkRect2D scissor{ .offset{ 0, 0 }, .extent{ kSunShadowResolution, kSunShadowResolution } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mShadowDepthPipeline);
            drawItems(mSunShadowViewProj, mSunShadowItems);

            vkCmdEndRendering(cmd);

            vk::ImageBarrier(cmd, mSunShadowImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }

        const bool anySpotActive = std::any_of(mSpotShadowCasters.begin(), mSpotShadowCasters.end(),
            [](const SpotShadowCaster &c) { return c.active; });
        if (anySpotActive)
        {
            vk::ImageBarrier(cmd, mSpotShadowImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                1, kMaxSpotShadows);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mShadowDepthPipeline);

            const VkViewport viewport{ .x = 0.0f, .y = 0.0f,
                .width = static_cast<float>(kSpotShadowResolution), .height = static_cast<float>(kSpotShadowResolution),
                .minDepth = 0.0f, .maxDepth = 1.0f };
            const VkRect2D scissor{ .offset{ 0, 0 }, .extent{ kSpotShadowResolution, kSpotShadowResolution } };

            for (uint32_t slot = 0; slot < kMaxSpotShadows; ++slot)
            {
                if (!mSpotShadowCasters[slot].active)
                    continue;

                const VkRenderingAttachmentInfo depthAttachment{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = mSpotShadowLayerViews[mFrameIndex][slot],
                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue{ .depthStencil{ .depth = 1.0f } } };

                const VkRenderingInfo renderingInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea{ .offset{ 0, 0 }, .extent{ kSpotShadowResolution, kSpotShadowResolution } },
                    .layerCount = 1,
                    .pDepthAttachment = &depthAttachment };

                vkCmdBeginRendering(cmd, &renderingInfo);
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                drawItems(mSpotShadowCasters[slot].viewProj, mSpotShadowCasters[slot].items);
                vkCmdEndRendering(cmd);
            }

            vk::ImageBarrier(cmd, mSpotShadowImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                1, kMaxSpotShadows);
        }

        const bool anyPointActive = std::any_of(mPointShadowCasters.begin(), mPointShadowCasters.end(),
            [](const PointShadowCaster &c) { return c.active; });
        if (anyPointActive)
        {
            constexpr uint32_t kPointLayers = kMaxPointShadows * 6;

            vk::ImageBarrier(cmd, mPointShadowDepthImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                1, kPointLayers);

            vk::ImageBarrier(cmd, mPointShadowColorImages[mFrameIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                1, kPointLayers);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPointShadowPipeline);

            const VkViewport viewport{ .x = 0.0f, .y = 0.0f,
                .width = static_cast<float>(kPointShadowResolution), .height = static_cast<float>(kPointShadowResolution),
                .minDepth = 0.0f, .maxDepth = 1.0f };
            const VkRect2D scissor{ .offset{ 0, 0 }, .extent{ kPointShadowResolution, kPointShadowResolution } };

            for (uint32_t slot = 0; slot < kMaxPointShadows; ++slot)
            {
                if (!mPointShadowCasters[slot].active)
                    continue;

                for (uint32_t face = 0; face < 6; ++face)
                {
                    const uint32_t layer = slot * 6 + face;

                    const VkRenderingAttachmentInfo colorAttachment{
                        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView = mPointShadowColorLayerViews[mFrameIndex][layer],
                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .clearValue{ .color{ { 1e6f, 0.0f, 0.0f, 0.0f } } } };

                    const VkRenderingAttachmentInfo depthAttachment{
                        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView = mPointShadowDepthLayerViews[mFrameIndex][layer],
                        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // never sampled afterward
                        .clearValue{ .depthStencil{ .depth = 1.0f } } };

                    const VkRenderingInfo renderingInfo{
                        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                        .renderArea{ .offset{ 0, 0 }, .extent{ kPointShadowResolution, kPointShadowResolution } },
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &colorAttachment,
                        .pDepthAttachment = &depthAttachment };

                    vkCmdBeginRendering(cmd, &renderingInfo);
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    vkCmdSetScissor(cmd, 0, 1, &scissor);
                    drawItems(mPointShadowCasters[slot].faces[face].viewProj, mPointShadowCasters[slot].faces[face].items);
                    vkCmdEndRendering(cmd);
                }
            }

            vk::ImageBarrier(cmd, mPointShadowColorImages[mFrameIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                1, kPointLayers);
        }
    }
}
