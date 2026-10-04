/**
 * @file VulkanRayTracing.cpp
 * @author Sumin Park
 * @brief Ray tracing: BLAS, TLAS, SBT, RT pipeline and the traced frame.
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
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstring>
#include <format>
#include <vector>

namespace mir
{
    namespace
    {
        struct RtInstanceInfo
        {
            VkDeviceAddress vertexAddress;
            VkDeviceAddress indexAddress;
            glm::vec4 tint;
            glm::vec4 normalCol0;
            glm::vec4 normalCol1;
            glm::vec4 normalCol2;
            uint32_t unlit;
            uint32_t pad[3];
        };

        VkTransformMatrixKHR ToTransformMatrixKHR(const glm::mat4 &m)
        {
            VkTransformMatrixKHR out{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    out.matrix[row][col] = m[col][row];
            return out;
        }

        VkDeviceAddress GetBufferAddress(VkDevice device, VkBuffer buffer)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = buffer };
            return vkGetBufferDeviceAddress(device, &info);
        }

        struct AccelBuildSizes
        {
            VkDeviceSize accelerationStructureSize = 0;
            VkDeviceSize buildScratchSize = 0;
        };

        AccelBuildSizes QueryAccelBuildSizes(VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo, uint32_t maxPrimitiveCount)
        {
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo, &maxPrimitiveCount, &sizes);
            return { sizes.accelerationStructureSize, sizes.buildScratchSize };
        }
    }

    bool VulkanRenderer::CreateRtOutputImages()
    {
        if (!mRayTracingSupported)
            return true;

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent{ mSwapchainExtent.width, mSwapchainExtent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
                &mRtOutputImages[i], &mRtOutputAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vmaCreateImage failed for RT output image {}", i);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mRtOutputImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice, &viewInfo, nullptr, &mRtOutputViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateImageView failed for RT output image {}", i);
                return false;
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mRtOutputImages[i]),
                std::format("RT output image {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyRtOutputImages()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mRtOutputViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mRtOutputViews[i], nullptr);
                mRtOutputViews[i] = VK_NULL_HANDLE;
            }
            if (mRtOutputImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mRtOutputImages[i], mRtOutputAllocations[i]);
                mRtOutputImages[i] = VK_NULL_HANDLE;
                mRtOutputAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    bool VulkanRenderer::CreateRtPipeline()
    {
        VkShaderModule raygenModule = vk::CreateShaderModule(mDevice, ShaderPath("raytrace.rayGenMain.spv"));
        VkShaderModule missModule = vk::CreateShaderModule(mDevice, ShaderPath("raytrace.missMain.spv"));
        VkShaderModule shadowMissModule = vk::CreateShaderModule(mDevice, ShaderPath("raytrace.shadowMissMain.spv"));
        VkShaderModule hitModule = vk::CreateShaderModule(mDevice, ShaderPath("raytrace.closestHitMain.spv"));

        if (raygenModule == VK_NULL_HANDLE || missModule == VK_NULL_HANDLE ||
            shadowMissModule == VK_NULL_HANDLE || hitModule == VK_NULL_HANDLE)
        {
            if (raygenModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, raygenModule, nullptr);
            if (missModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, missModule, nullptr);
            if (shadowMissModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
            if (hitModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, hitModule, nullptr);
            return false;
        }

        const std::array<VkDescriptorSetLayoutBinding, 3> setBindings{ {
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR },
            { .binding = 2,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR } } };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(setBindings.size()),
            .pBindings = setBindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice, &setLayoutInfo, nullptr, &mRtSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: vkCreateDescriptorSetLayout failed");
            vkDestroyShaderModule(mDevice, raygenModule, nullptr);
            vkDestroyShaderModule(mDevice, missModule, nullptr);
            vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice, hitModule, nullptr);
            return false;
        }

        const std::array<VkDescriptorPoolSize, 3> poolSizes{ {
            { .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, .descriptorCount = kFramesInFlight },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = kFramesInFlight },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = kFramesInFlight } } };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data() };

        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mRtDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: vkCreateDescriptorPool failed");
            vkDestroyShaderModule(mDevice, raygenModule, nullptr);
            vkDestroyShaderModule(mDevice, missModule, nullptr);
            vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice, hitModule, nullptr);
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mRtDescriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mRtSetLayout };

            if (vkAllocateDescriptorSets(mDevice, &setAllocInfo, &mRtDescriptorSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateRtPipeline: descriptor set allocation failed for frame {}", i);
                vkDestroyShaderModule(mDevice, raygenModule, nullptr);
                vkDestroyShaderModule(mDevice, missModule, nullptr);
                vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
                vkDestroyShaderModule(mDevice, hitModule, nullptr);
                return false;
            }
        }

        const std::array<VkDescriptorSetLayout, 2> setLayouts{ mRtSetLayout, mCameraSetLayout };

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .offset = 0,
            .size = sizeof(vk::RtPushConstants) };

        const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice, &pipelineLayoutInfo, nullptr, &mRtPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: vkCreatePipelineLayout failed");
            vkDestroyShaderModule(mDevice, raygenModule, nullptr);
            vkDestroyShaderModule(mDevice, missModule, nullptr);
            vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice, hitModule, nullptr);
            return false;
        }

        const std::array<VkPipelineShaderStageCreateInfo, 4> stages{ {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
             .module = raygenModule,
             .pName = "rayGenMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
             .module = missModule,
             .pName = "missMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
             .module = shadowMissModule,
             .pName = "shadowMissMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
             .module = hitModule,
             .pName = "closestHitMain" } } };

        const std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups{ {
            { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
             .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
             .generalShader = 0,
             .closestHitShader = VK_SHADER_UNUSED_KHR,
             .anyHitShader = VK_SHADER_UNUSED_KHR,
             .intersectionShader = VK_SHADER_UNUSED_KHR },
            { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
             .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
             .generalShader = 1,
             .closestHitShader = VK_SHADER_UNUSED_KHR,
             .anyHitShader = VK_SHADER_UNUSED_KHR,
             .intersectionShader = VK_SHADER_UNUSED_KHR },
            { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
             .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
             .generalShader = 2,
             .closestHitShader = VK_SHADER_UNUSED_KHR,
             .anyHitShader = VK_SHADER_UNUSED_KHR,
             .intersectionShader = VK_SHADER_UNUSED_KHR },
            { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
             .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
             .generalShader = VK_SHADER_UNUSED_KHR,
             .closestHitShader = 3,
             .anyHitShader = VK_SHADER_UNUSED_KHR,
             .intersectionShader = VK_SHADER_UNUSED_KHR } } };

        const VkRayTracingPipelineCreateInfoKHR pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .stageCount = static_cast<uint32_t>(stages.size()),
            .pStages = stages.data(),
            .groupCount = static_cast<uint32_t>(groups.size()),
            .pGroups = groups.data(),
            .maxPipelineRayRecursionDepth = 2,
            .layout = mRtPipelineLayout };

        const VkResult pipelineResult = vkCreateRayTracingPipelinesKHR(
            mDevice, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mRtPipeline);

        vkDestroyShaderModule(mDevice, raygenModule, nullptr);
        vkDestroyShaderModule(mDevice, missModule, nullptr);
        vkDestroyShaderModule(mDevice, shadowMissModule, nullptr);
        vkDestroyShaderModule(mDevice, hitModule, nullptr);

        if (pipelineResult != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: vkCreateRayTracingPipelinesKHR failed: {}", static_cast<int>(pipelineResult));
            return false;
        }

        constexpr uint32_t kGroupCount = 4;
        const uint32_t handleSize = mRtProperties.shaderGroupHandleSize;
        const uint32_t handleSizeAligned = vk::AlignUp(handleSize, mRtProperties.shaderGroupHandleAlignment);
        const VkDeviceSize regionSize = vk::AlignUp(handleSizeAligned, mRtProperties.shaderGroupBaseAlignment);
        const VkDeviceSize sbtSize = regionSize * kGroupCount;

        std::vector<std::byte> handles(static_cast<size_t>(handleSize) * kGroupCount);
        if (vkGetRayTracingShaderGroupHandlesKHR(mDevice, mRtPipeline, 0, kGroupCount,
            handles.size(), handles.data()) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: vkGetRayTracingShaderGroupHandlesKHR failed");
            return false;
        }

        const VkBufferCreateInfo sbtBufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sbtSize,
            .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };

        const VmaAllocationCreateInfo sbtAllocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO };

        VmaAllocationInfo sbtInfo{};
        if (vmaCreateBufferWithAlignment(mAllocator, &sbtBufferInfo, &sbtAllocInfo,
            mRtProperties.shaderGroupBaseAlignment, &mSbtBuffer, &mSbtAllocation, &sbtInfo) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRtPipeline: SBT buffer allocation failed");
            return false;
        }

        std::byte *const sbtMapped = static_cast<std::byte *>(sbtInfo.pMappedData);
        for (uint32_t i = 0; i < kGroupCount; ++i)
            std::memcpy(sbtMapped + i * regionSize, handles.data() + i * handleSize, handleSize);

        const VkDeviceAddress sbtBase = GetBufferAddress(mDevice, mSbtBuffer);
        mSbtRaygenRegion = { .deviceAddress = sbtBase + 0 * regionSize, .stride = regionSize, .size = regionSize };
        mSbtMissRegion = { .deviceAddress = sbtBase + 1 * regionSize, .stride = regionSize, .size = 2 * regionSize };
        mSbtHitRegion = { .deviceAddress = sbtBase + 3 * regionSize, .stride = regionSize, .size = regionSize };
        mSbtCallableRegion = {};

        return true;
    }

    void VulkanRenderer::WriteRtDescriptorSets()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkWriteDescriptorSetAccelerationStructureKHR accelWrite{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &mTlas[i] };

            const VkDescriptorImageInfo imageInfo{
                .imageView = mRtOutputViews[i],
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL };

            const VkDescriptorBufferInfo instanceInfoBufferInfo{
                .buffer = mRtInstanceInfoBuffers[i],
                .offset = 0,
                .range = VK_WHOLE_SIZE };

            const std::array<VkWriteDescriptorSet, 3> writes{ {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .pNext = &accelWrite,
                 .dstSet = mRtDescriptorSets[i],
                 .dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mRtDescriptorSets[i],
                 .dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &imageInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mRtDescriptorSets[i],
                 .dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &instanceInfoBufferInfo } } };

            vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    bool VulkanRenderer::CreateRtScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VmaAllocationInfo instanceInfo{};
            if (!vk::CreateBuffer(mAllocator, sizeof(VkAccelerationStructureInstanceKHR) * kMaxRtInstances,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mInstanceBuffers[i], mInstanceAllocations[i], &instanceInfo))
            {
                MIR_LOG_CRITICAL("CreateRtScene: instance buffer allocation failed for frame {}", i);
                return false;
            }
            mInstanceMapped[i] = static_cast<std::byte *>(instanceInfo.pMappedData);

            VmaAllocationInfo instanceRtInfo{};
            if (!vk::CreateBuffer(mAllocator, sizeof(RtInstanceInfo) * kMaxRtInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mRtInstanceInfoBuffers[i], mRtInstanceInfoAllocations[i], &instanceRtInfo))
            {
                MIR_LOG_CRITICAL("CreateRtScene: instance info buffer allocation failed for frame {}", i);
                return false;
            }
            mRtInstanceInfoMapped[i] = static_cast<std::byte *>(instanceRtInfo.pMappedData);

            const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE };

            const VkAccelerationStructureGeometryKHR geometry{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .geometry{ .instances = instancesData } };

            const VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1,
                .pGeometries = &geometry };

            const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice, buildInfo, kMaxRtInstances);

            if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
                VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                mTlasBuffers[i], mTlasAllocations[i], mTlas[i]))
            {
                MIR_LOG_CRITICAL("CreateRtScene: TLAS creation failed for frame {}", i);
                return false;
            }

            if (!vk::CreateBuffer(mAllocator, sizes.buildScratchSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                0, mTlasScratchBuffers[i], mTlasScratchAllocations[i], nullptr))
            {
                MIR_LOG_CRITICAL("CreateRtScene: TLAS scratch buffer allocation failed for frame {}", i);
                return false;
            }
            mTlasScratchAddresses[i] = GetBufferAddress(mDevice, mTlasScratchBuffers[i]);

            NameObject(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(mTlas[i]),
                std::format("TLAS {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyRtScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mTlas[i] != VK_NULL_HANDLE)
            {
                vkDestroyAccelerationStructureKHR(mDevice, mTlas[i], nullptr);
                mTlas[i] = VK_NULL_HANDLE;
            }
            if (mTlasBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mTlasBuffers[i], mTlasAllocations[i]);
                mTlasBuffers[i] = VK_NULL_HANDLE;
                mTlasAllocations[i] = VK_NULL_HANDLE;
            }
            if (mTlasScratchBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mTlasScratchBuffers[i], mTlasScratchAllocations[i]);
                mTlasScratchBuffers[i] = VK_NULL_HANDLE;
                mTlasScratchAllocations[i] = VK_NULL_HANDLE;
            }
            if (mInstanceBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mInstanceBuffers[i], mInstanceAllocations[i]);
                mInstanceBuffers[i] = VK_NULL_HANDLE;
                mInstanceAllocations[i] = VK_NULL_HANDLE;
                mInstanceMapped[i] = nullptr;
            }
            if (mRtInstanceInfoBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mRtInstanceInfoBuffers[i], mRtInstanceInfoAllocations[i]);
                mRtInstanceInfoBuffers[i] = VK_NULL_HANDLE;
                mRtInstanceInfoAllocations[i] = VK_NULL_HANDLE;
                mRtInstanceInfoMapped[i] = nullptr;
            }
            mTlasScratchAddresses[i] = 0;
        }
    }

    void VulkanRenderer::RecordTlasBuild(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const DrawItem> items)
    {
        auto *const instances = reinterpret_cast<VkAccelerationStructureInstanceKHR *>(mInstanceMapped[frameIndex]);
        auto *const instanceInfos = reinterpret_cast<RtInstanceInfo *>(mRtInstanceInfoMapped[frameIndex]);

        uint32_t instanceCount = 0;
        for (const DrawItem &item : items)
        {
            if (instanceCount >= kMaxRtInstances)
            {
                if (!mWarnedRtInstanceCap)
                {
                    MIR_LOG_WARN("RecordTlasBuild: more than {} draws; the rest are not traced", kMaxRtInstances);
                    mWarnedRtInstanceCap = true;
                }
                break;
            }

            const GpuMesh *const mesh = FindMesh(item.mesh);
            if (mesh == nullptr || mesh->mBlas == VK_NULL_HANDLE)
                continue;

            instances[instanceCount] = VkAccelerationStructureInstanceKHR{
                .transform = ToTransformMatrixKHR(item.model),
                .instanceCustomIndex = instanceCount,
                .mask = 0xFF,
                .instanceShaderBindingTableRecordOffset = 0,
                .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
                .accelerationStructureReference = mesh->mBlasAddress };

            const GpuMaterial *material = FindMaterial(item.material);
            if (material == nullptr)
                material = FindMaterial(mDefaultMaterial);

            instanceInfos[instanceCount] = RtInstanceInfo{
                .vertexAddress = GetBufferAddress(mDevice, mesh->mVertexBuffer),
                .indexAddress = GetBufferAddress(mDevice, mesh->mIndexBuffer),
                .tint = item.tint,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .unlit = (material != nullptr && material->mUnlit) ? 1u : 0u };

            ++instanceCount;
        }

        const VkDeviceSize instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * instanceCount;
        if (instanceBytes > 0)
            vmaFlushAllocation(mAllocator, mInstanceAllocations[frameIndex], 0, instanceBytes);

        const VkDeviceSize instanceInfoBytes = sizeof(RtInstanceInfo) * instanceCount;
        if (instanceInfoBytes > 0)
            vmaFlushAllocation(mAllocator, mRtInstanceInfoAllocations[frameIndex], 0, instanceInfoBytes);

        const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE,
            .data{ .deviceAddress = GetBufferAddress(mDevice, mInstanceBuffers[frameIndex]) } };

        const VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry{ .instances = instancesData } };

        const VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .dstAccelerationStructure = mTlas[frameIndex],
            .geometryCount = 1,
            .pGeometries = &geometry,
            .scratchData{ .deviceAddress = mTlasScratchAddresses[frameIndex] } };

        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = instanceCount };
        const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
    }

    bool VulkanRenderer::CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
        VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS)
    {
        if (!vk::CreateBuffer(mAllocator, size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0, outBuffer, outAllocation, nullptr))
            return false;

        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = outBuffer,
            .size = size,
            .type = type };

        if (vkCreateAccelerationStructureKHR(mDevice, &createInfo, nullptr, &outAS) != VK_SUCCESS)
        {
            vmaDestroyBuffer(mAllocator, outBuffer, outAllocation);
            outBuffer = VK_NULL_HANDLE;
            outAllocation = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void VulkanRenderer::DestroyBLAS(GpuMesh &mesh)
    {
        if (mesh.mBlas != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(mDevice, mesh.mBlas, nullptr);
            mesh.mBlas = VK_NULL_HANDLE;
        }
        if (mesh.mBlasBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mAllocator, mesh.mBlasBuffer, mesh.mBlasAllocation);
            mesh.mBlasBuffer = VK_NULL_HANDLE;
            mesh.mBlasAllocation = VK_NULL_HANDLE;
        }
        mesh.mBlasAddress = 0;
    }

    bool VulkanRenderer::BuildBLAS(GpuMesh &mesh, uint32_t vertexCount, uint32_t indexCount, VkCommandBuffer cmd,
        VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation)
    {
        const VkDeviceAddress vertexAddress = GetBufferAddress(mDevice, mesh.mVertexBuffer);
        const VkDeviceAddress indexAddress = GetBufferAddress(mDevice, mesh.mIndexBuffer);

        const VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData{ .deviceAddress = vertexAddress },
            .vertexStride = sizeof(Vertex),
            .maxVertex = vertexCount - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData{ .deviceAddress = indexAddress } };

        const VkAccelerationStructureGeometryKHR geometry{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry{ .triangles = triangles },
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR };

        const uint32_t primitiveCount = indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries = &geometry };

        const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice, buildInfo, primitiveCount);

        if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            mesh.mBlasBuffer, mesh.mBlasAllocation, mesh.mBlas))
        {
            MIR_LOG_CRITICAL("BuildBLAS: acceleration structure creation failed");
            return false;
        }

        if (!vk::CreateBuffer(mAllocator, sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0, outScratchBuffer, outScratchAllocation, nullptr))
        {
            MIR_LOG_CRITICAL("BuildBLAS: scratch buffer allocation failed");
            DestroyBLAS(mesh);
            return false;
        }

        buildInfo.dstAccelerationStructure = mesh.mBlas;
        buildInfo.scratchData.deviceAddress = GetBufferAddress(mDevice, outScratchBuffer);

        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = primitiveCount };
        const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

        const VkMemoryBarrier2 preBuild{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT };
        const VkDependencyInfo preBuildDep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &preBuild };
        vkCmdPipelineBarrier2(cmd, &preBuildDep);

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);

        const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = mesh.mBlas };
        mesh.mBlasAddress = vkGetAccelerationStructureDeviceAddressKHR(mDevice, &addressInfo);

        return true;
    }

    void VulkanRenderer::RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        const RenderView &view = views[0];

        const VkRect2D rect = vk::ResolveViewRect(view.rect, mSwapchainExtent);

        WriteCameraData(view, 0);
        const uint32_t cameraOffset = 0;

        if (mValidationEnabled)
        {
            const VkDebugUtilsLabelEXT label{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = "Ray Trace Pass",
                .color{ 0.0f, 1.0f, 1.0f, 1.0f } };
            vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
        }

        vk::ImageBarrier(cmd, mRtOutputImages[mFrameIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        RecordTlasBuild(cmd, mFrameIndex, view.items);

        const VkMemoryBarrier2 tlasBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        const VkDependencyInfo tlasDep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &tlasBarrier };
        vkCmdPipelineBarrier2(cmd, &tlasDep);

        if (rect.extent.width > 0 && rect.extent.height > 0)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mRtPipeline);

            const std::array<VkDescriptorSet, 2> sets{ mRtDescriptorSets[mFrameIndex], mCameraDescriptorSets[mFrameIndex] };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mRtPipelineLayout,
                0, static_cast<uint32_t>(sets.size()), sets.data(), 1, &cameraOffset);

            const vk::RtPushConstants pushConstants{
                .outputOffset{ static_cast<uint32_t>(rect.offset.x), static_cast<uint32_t>(rect.offset.y) } };
            vkCmdPushConstants(cmd, mRtPipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                0, sizeof(vk::RtPushConstants), &pushConstants);

            vkCmdTraceRaysKHR(cmd, &mSbtRaygenRegion, &mSbtMissRegion, &mSbtHitRegion, &mSbtCallableRegion,
                rect.extent.width, rect.extent.height, 1);
        }

        vk::ImageBarrier(cmd, mRtOutputImages[mFrameIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        vk::ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkClearColorValue clearColor{ .float32{ mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } };
        const VkImageSubresourceRange wholeImage{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
        vkCmdClearColorImage(cmd, mSwapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor, 1, &wholeImage);

        vk::ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
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
                mRtOutputImages[mFrameIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                mSwapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blitRegion, VK_FILTER_NEAREST);
        }

        if (mImguiDrawData != nullptr)
        {
            vk::ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            const VkRenderingAttachmentInfo colorAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mSwapchainViews[imageIndex],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE };

            const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent = mSwapchainExtent },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &colorAttachment };

            vkCmdBeginRendering(cmd, &renderingInfo);
            ImGui_ImplVulkan_RenderDrawData(mImguiDrawData, cmd);
            vkCmdEndRendering(cmd);

            vk::ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
        }
        else
        {
            vk::ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
        }

        if (mValidationEnabled)
            vkCmdEndDebugUtilsLabelEXT(cmd);
    }

    void VulkanRenderer::TraceFrame(std::span<const RenderView> views)
    {
        if (!mRayTracingSupported)
        {
            MIR_LOG_ERROR("TraceFrame: called without ray tracing support; ignored");
            return;
        }

        if (views.empty())
            return;

        if (mWindow->Width() == 0 || mWindow->Height() == 0)
            return;

        if (mNeedRecreate || mWindow->Height() != mSwapchainExtent.height || mWindow->Width() != mSwapchainExtent.width)
        {
            if (!RecreateSwapchain())
                return;
        }

        const uint64_t signalValue = mNextSignalValue++;
        const uint64_t waitValue = signalValue - kFramesInFlight;

        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &mTimeline,
            .pValues = &waitValue };

        vkWaitSemaphores(mDevice, &waitInfo, UINT64_MAX);

        vkResetCommandPool(mDevice, mCmdPools[mFrameIndex], 0);

        uint32_t imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(
            mDevice, mSwapchain, UINT64_MAX,
            mImageAcquired[mFrameIndex], VK_NULL_HANDLE, &imageIndex);

        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            mNeedRecreate = true;
            --mNextSignalValue;
            return;
        }

        if (acquireResult == VK_SUBOPTIMAL_KHR)
        {
            mNeedRecreate = true;
        }
        else if (acquireResult != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkAcquireNextImageKHR failed: {}", static_cast<int>(acquireResult));
            --mNextSignalValue;
            return;
        }

        VkCommandBuffer cmd = mCmdBuffers[mFrameIndex];

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

        vkBeginCommandBuffer(cmd, &beginInfo);
        RecordRtFrame(cmd, imageIndex, views);
        vkEndCommandBuffer(cmd);

        const VkSemaphoreSubmitInfo waitSem{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mImageAcquired[mFrameIndex],
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };

        const VkSemaphoreSubmitInfo signalSems[]{
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = mRenderComplete[imageIndex],
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = mTimeline,
             .value = signalValue,
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT } };

        const VkCommandBufferSubmitInfo cmdInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cmd };

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitSem,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos = signalSems };

        if (vkQueueSubmit2(mGfxQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkQueueSubmit2 failed");
            return;
        }
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &mRenderComplete[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &mSwapchain,
            .pImageIndices = &imageIndex };

        const VkResult presentResult = vkQueuePresentKHR(mGfxQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            mNeedRecreate = true;
        else if (presentResult != VK_SUCCESS)
            MIR_LOG_ERROR("vkQueuePresentKHR failed: {}", static_cast<int>(presentResult));

        mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
    }
}
