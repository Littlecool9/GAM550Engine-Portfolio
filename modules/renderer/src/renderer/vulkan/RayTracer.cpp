/**
 * @file RayTracer.cpp
 * @author Sumin Park
 * @brief Ray tracing: BLAS, TLAS, SBT, RT pipeline and output images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "RayTracer.h"
#include "Device.h"
#include "MaterialStore.h"
#include "ResourceStore.h"
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/math/Align.h>

#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>
#include <cstring>
#include <format>
#include <vector>

namespace mir::vk
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

    bool RayTracer::Initialize(Device &device, VkExtent2D extent, VkDescriptorSetLayout cameraSetLayout)
    {
        mDevice = &device;

        if (!CreateOutputImages(extent) || !CreateScene() || !CreatePipeline(cameraSetLayout))
            return false;

        WriteDescriptorSets();
        return true;
    }

    void RayTracer::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyBlases();
        DestroyScene();

        if (mSbtBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), mSbtBuffer, mSbtAllocation);
            mSbtBuffer = VK_NULL_HANDLE;
            mSbtAllocation = VK_NULL_HANDLE;
        }

        if (mPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mPipeline, nullptr);
            mPipeline = VK_NULL_HANDLE;
        }

        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }

        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }

        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }

        DestroyOutputImages();
    }

    bool RayTracer::Resize(VkExtent2D extent)
    {
        DestroyOutputImages();
        if (!CreateOutputImages(extent))
            return false;

        WriteDescriptorSets();
        return true;
    }

    bool RayTracer::BuildMeshBlas(uint32_t meshIndex, const GpuMesh &mesh, uint32_t vertexCount)
    {
        if (mBlases.size() <= meshIndex)
            mBlases.resize(meshIndex + 1);
        MeshBlas &blas = mBlases[meshIndex];

        VkBuffer scratch = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = VK_NULL_HANDLE;
        bool built = false;
        const bool submitted = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                built = BuildBLAS(mesh, vertexCount, cmd, blas, scratch, scratchAllocation);
            });

        if (scratch != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), scratch, scratchAllocation);

        if (!built || !submitted)
        {
            DestroyBLAS(blas);
            return false;
        }
        return true;
    }

    bool RayTracer::CreateOutputImages(VkExtent2D extent)
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent{ extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
                &mOutputImages[i], &mOutputAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateOutputImages: vmaCreateImage failed for RT output image {}", i);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mOutputImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &mOutputViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateOutputImages: vkCreateImageView failed for RT output image {}", i);
                return false;
            }

            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mOutputImages[i]),
                std::format("RT output image {}", i).c_str());
        }

        return true;
    }

    void RayTracer::DestroyOutputImages()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mOutputViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice->Handle(), mOutputViews[i], nullptr);
                mOutputViews[i] = VK_NULL_HANDLE;
            }
            if (mOutputImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mDevice->Allocator(), mOutputImages[i], mOutputAllocations[i]);
                mOutputImages[i] = VK_NULL_HANDLE;
                mOutputAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    bool RayTracer::CreatePipeline(VkDescriptorSetLayout cameraSetLayout)
    {
        VkShaderModule raygenModule = CreateShaderModule(mDevice->Handle(), ShaderPath("raytrace.rayGenMain.spv"));
        VkShaderModule missModule = CreateShaderModule(mDevice->Handle(), ShaderPath("raytrace.missMain.spv"));
        VkShaderModule shadowMissModule = CreateShaderModule(mDevice->Handle(), ShaderPath("raytrace.shadowMissMain.spv"));
        VkShaderModule hitModule = CreateShaderModule(mDevice->Handle(), ShaderPath("raytrace.closestHitMain.spv"));

        if (raygenModule == VK_NULL_HANDLE || missModule == VK_NULL_HANDLE ||
            shadowMissModule == VK_NULL_HANDLE || hitModule == VK_NULL_HANDLE)
        {
            if (raygenModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
            if (missModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
            if (shadowMissModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
            if (hitModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);
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

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateDescriptorSetLayout failed");
            vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);
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

        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateDescriptorPool failed");
            vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mSetLayout };

            if (vkAllocateDescriptorSets(mDevice->Handle(), &setAllocInfo, &mSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreatePipeline: descriptor set allocation failed for frame {}", i);
                vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
                vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
                vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
                vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);
                return false;
            }
        }

        const std::array<VkDescriptorSetLayout, 2> setLayouts{ mSetLayout, cameraSetLayout };

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .offset = 0,
            .size = sizeof(RtPushConstants) };

        const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice->Handle(), &pipelineLayoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreatePipelineLayout failed");
            vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);
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
            .layout = mPipelineLayout };

        const VkResult pipelineResult = vkCreateRayTracingPipelinesKHR(
            mDevice->Handle(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mPipeline);

        vkDestroyShaderModule(mDevice->Handle(), raygenModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), missModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), shadowMissModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), hitModule, nullptr);

        if (pipelineResult != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkCreateRayTracingPipelinesKHR failed: {}", static_cast<int>(pipelineResult));
            return false;
        }

        constexpr uint32_t kGroupCount = 4;
        const uint32_t handleSize = mDevice->RtProperties().shaderGroupHandleSize;
        const uint32_t handleSizeAligned = AlignUp(handleSize, mDevice->RtProperties().shaderGroupHandleAlignment);
        const VkDeviceSize regionSize = AlignUp(handleSizeAligned, mDevice->RtProperties().shaderGroupBaseAlignment);
        const VkDeviceSize sbtSize = regionSize * kGroupCount;

        std::vector<std::byte> handles(static_cast<size_t>(handleSize) * kGroupCount);
        if (vkGetRayTracingShaderGroupHandlesKHR(mDevice->Handle(), mPipeline, 0, kGroupCount,
            handles.size(), handles.data()) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: vkGetRayTracingShaderGroupHandlesKHR failed");
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
        if (vmaCreateBufferWithAlignment(mDevice->Allocator(), &sbtBufferInfo, &sbtAllocInfo,
            mDevice->RtProperties().shaderGroupBaseAlignment, &mSbtBuffer, &mSbtAllocation, &sbtInfo) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipeline: SBT buffer allocation failed");
            return false;
        }

        std::byte *const sbtMapped = static_cast<std::byte *>(sbtInfo.pMappedData);
        for (uint32_t i = 0; i < kGroupCount; ++i)
            std::memcpy(sbtMapped + i * regionSize, handles.data() + i * handleSize, handleSize);

        const VkDeviceAddress sbtBase = GetBufferAddress(mDevice->Handle(), mSbtBuffer);
        mSbtRaygenRegion = { .deviceAddress = sbtBase + 0 * regionSize, .stride = regionSize, .size = regionSize };
        mSbtMissRegion = { .deviceAddress = sbtBase + 1 * regionSize, .stride = regionSize, .size = 2 * regionSize };
        mSbtHitRegion = { .deviceAddress = sbtBase + 3 * regionSize, .stride = regionSize, .size = regionSize };
        mSbtCallableRegion = {};

        return true;
    }

    void RayTracer::WriteDescriptorSets()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkWriteDescriptorSetAccelerationStructureKHR accelWrite{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &mTlas[i] };

            const VkDescriptorImageInfo imageInfo{
                .imageView = mOutputViews[i],
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL };

            const VkDescriptorBufferInfo instanceInfoBufferInfo{
                .buffer = mInstanceInfoBuffers[i],
                .offset = 0,
                .range = VK_WHOLE_SIZE };

            const std::array<VkWriteDescriptorSet, 3> writes{ {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .pNext = &accelWrite,
                 .dstSet = mSets[i],
                 .dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mSets[i],
                 .dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &imageInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mSets[i],
                 .dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &instanceInfoBufferInfo } } };

            vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    bool RayTracer::CreateScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VmaAllocationInfo instanceInfo{};
            if (!CreateBuffer(mDevice->Allocator(), sizeof(VkAccelerationStructureInstanceKHR) * kMaxInstances,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mInstanceBuffers[i], mInstanceAllocations[i], &instanceInfo))
            {
                MIR_LOG_CRITICAL("CreateScene: instance buffer allocation failed for frame {}", i);
                return false;
            }
            mInstanceMapped[i] = static_cast<std::byte *>(instanceInfo.pMappedData);

            VmaAllocationInfo instanceRtInfo{};
            if (!CreateBuffer(mDevice->Allocator(), sizeof(RtInstanceInfo) * kMaxInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mInstanceInfoBuffers[i], mInstanceInfoAllocations[i], &instanceRtInfo))
            {
                MIR_LOG_CRITICAL("CreateScene: instance info buffer allocation failed for frame {}", i);
                return false;
            }
            mInstanceInfoMapped[i] = static_cast<std::byte *>(instanceRtInfo.pMappedData);

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

            const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), buildInfo, kMaxInstances);

            if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
                VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                mTlasBuffers[i], mTlasAllocations[i], mTlas[i]))
            {
                MIR_LOG_CRITICAL("CreateScene: TLAS creation failed for frame {}", i);
                return false;
            }

            if (!CreateBuffer(mDevice->Allocator(), sizes.buildScratchSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                0, mTlasScratchBuffers[i], mTlasScratchAllocations[i], nullptr))
            {
                MIR_LOG_CRITICAL("CreateScene: TLAS scratch buffer allocation failed for frame {}", i);
                return false;
            }
            mTlasScratchAddresses[i] = GetBufferAddress(mDevice->Handle(), mTlasScratchBuffers[i]);

            mDevice->NameObject(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(mTlas[i]),
                std::format("TLAS {}", i).c_str());
        }

        return true;
    }

    void RayTracer::DestroyScene()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mTlas[i] != VK_NULL_HANDLE)
            {
                vkDestroyAccelerationStructureKHR(mDevice->Handle(), mTlas[i], nullptr);
                mTlas[i] = VK_NULL_HANDLE;
            }
            if (mTlasBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mTlasBuffers[i], mTlasAllocations[i]);
                mTlasBuffers[i] = VK_NULL_HANDLE;
                mTlasAllocations[i] = VK_NULL_HANDLE;
            }
            if (mTlasScratchBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mTlasScratchBuffers[i], mTlasScratchAllocations[i]);
                mTlasScratchBuffers[i] = VK_NULL_HANDLE;
                mTlasScratchAllocations[i] = VK_NULL_HANDLE;
            }
            if (mInstanceBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mInstanceBuffers[i], mInstanceAllocations[i]);
                mInstanceBuffers[i] = VK_NULL_HANDLE;
                mInstanceAllocations[i] = VK_NULL_HANDLE;
                mInstanceMapped[i] = nullptr;
            }
            if (mInstanceInfoBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mInstanceInfoBuffers[i], mInstanceInfoAllocations[i]);
                mInstanceInfoBuffers[i] = VK_NULL_HANDLE;
                mInstanceInfoAllocations[i] = VK_NULL_HANDLE;
                mInstanceInfoMapped[i] = nullptr;
            }
            mTlasScratchAddresses[i] = 0;
        }
    }

    void RayTracer::RecordTlasBuild(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const DrawItem> items,
        const ResourceStore &resources, const MaterialStore &materials)
    {
        auto *const instances = reinterpret_cast<VkAccelerationStructureInstanceKHR *>(mInstanceMapped[frameIndex]);
        auto *const instanceInfos = reinterpret_cast<RtInstanceInfo *>(mInstanceInfoMapped[frameIndex]);

        uint32_t instanceCount = 0;
        for (const DrawItem &item : items)
        {
            if (instanceCount >= kMaxInstances)
            {
                if (!mWarnedInstanceCap)
                {
                    MIR_LOG_WARN("RecordTlasBuild: more than {} draws; the rest are not traced", kMaxInstances);
                    mWarnedInstanceCap = true;
                }
                break;
            }

            const GpuMesh *const mesh = resources.FindMesh(item.mesh);
            if (mesh == nullptr || item.mesh.mIndex >= mBlases.size() || mBlases[item.mesh.mIndex].mBlas == VK_NULL_HANDLE)
                continue;

            instances[instanceCount] = VkAccelerationStructureInstanceKHR{
                .transform = ToTransformMatrixKHR(item.model),
                .instanceCustomIndex = instanceCount,
                .mask = 0xFF,
                .instanceShaderBindingTableRecordOffset = 0,
                .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
                .accelerationStructureReference = mBlases[item.mesh.mIndex].mAddress };

            const GpuMaterial *const material = materials.FindOrDefault(item.material);

            instanceInfos[instanceCount] = RtInstanceInfo{
                .vertexAddress = GetBufferAddress(mDevice->Handle(), mesh->mVertexBuffer),
                .indexAddress = GetBufferAddress(mDevice->Handle(), mesh->mIndexBuffer),
                .tint = item.tint,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .unlit = (material != nullptr && material->mUnlit) ? 1u : 0u };

            ++instanceCount;
        }

        const VkDeviceSize instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * instanceCount;
        if (instanceBytes > 0)
            vmaFlushAllocation(mDevice->Allocator(), mInstanceAllocations[frameIndex], 0, instanceBytes);

        const VkDeviceSize instanceInfoBytes = sizeof(RtInstanceInfo) * instanceCount;
        if (instanceInfoBytes > 0)
            vmaFlushAllocation(mDevice->Allocator(), mInstanceInfoAllocations[frameIndex], 0, instanceInfoBytes);

        const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE,
            .data{ .deviceAddress = GetBufferAddress(mDevice->Handle(), mInstanceBuffers[frameIndex]) } };

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

    bool RayTracer::CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
        VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS)
    {
        if (!CreateBuffer(mDevice->Allocator(), size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0, outBuffer, outAllocation, nullptr))
            return false;

        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = outBuffer,
            .size = size,
            .type = type };

        if (vkCreateAccelerationStructureKHR(mDevice->Handle(), &createInfo, nullptr, &outAS) != VK_SUCCESS)
        {
            vmaDestroyBuffer(mDevice->Allocator(), outBuffer, outAllocation);
            outBuffer = VK_NULL_HANDLE;
            outAllocation = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void RayTracer::DestroyBLAS(MeshBlas &blas)
    {
        if (blas.mBlas != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(mDevice->Handle(), blas.mBlas, nullptr);
            blas.mBlas = VK_NULL_HANDLE;
        }
        if (blas.mBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), blas.mBuffer, blas.mAllocation);
            blas.mBuffer = VK_NULL_HANDLE;
            blas.mAllocation = VK_NULL_HANDLE;
        }
        blas.mAddress = 0;
    }

    void RayTracer::DestroyMeshBlas(uint32_t meshIndex)
    {
        if (meshIndex < mBlases.size())
            DestroyBLAS(mBlases[meshIndex]);
    }

    void RayTracer::DestroyBlases()
    {
        for (MeshBlas &blas : mBlases)
            DestroyBLAS(blas);
        mBlases.clear();
    }

    bool RayTracer::BuildBLAS(const GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
        VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation)
    {
        const VkDeviceAddress vertexAddress = GetBufferAddress(mDevice->Handle(), mesh.mVertexBuffer);
        const VkDeviceAddress indexAddress = GetBufferAddress(mDevice->Handle(), mesh.mIndexBuffer);

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

        const uint32_t primitiveCount = mesh.mIndexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries = &geometry };

        const AccelBuildSizes sizes = QueryAccelBuildSizes(mDevice->Handle(), buildInfo, primitiveCount);

        if (!CreateAccelerationStructureObject(sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            outBlas.mBuffer, outBlas.mAllocation, outBlas.mBlas))
        {
            MIR_LOG_CRITICAL("BuildBLAS: acceleration structure creation failed");
            return false;
        }

        if (!CreateBuffer(mDevice->Allocator(), sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0, outScratchBuffer, outScratchAllocation, nullptr))
        {
            MIR_LOG_CRITICAL("BuildBLAS: scratch buffer allocation failed");
            DestroyBLAS(outBlas);
            return false;
        }

        buildInfo.dstAccelerationStructure = outBlas.mBlas;
        buildInfo.scratchData.deviceAddress = GetBufferAddress(mDevice->Handle(), outScratchBuffer);

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
            .accelerationStructure = outBlas.mBlas };
        outBlas.mAddress = vkGetAccelerationStructureDeviceAddressKHR(mDevice->Handle(), &addressInfo);

        return true;
    }

    void RayTracer::Record(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items, const VkRect2D &rect,
        VkDescriptorSet cameraSet, uint32_t cameraOffset, const ResourceStore &resources,
        const MaterialStore &materials)
    {
        ImageBarrier(cmd, mOutputImages[frame], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        RecordTlasBuild(cmd, frame, items, resources, materials);

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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mPipeline);

            const std::array<VkDescriptorSet, 2> sets{ mSets[frame], cameraSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mPipelineLayout,
                0, static_cast<uint32_t>(sets.size()), sets.data(), 1, &cameraOffset);

            const RtPushConstants pushConstants{
                .outputOffset{ static_cast<uint32_t>(rect.offset.x), static_cast<uint32_t>(rect.offset.y) } };
            vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                0, sizeof(RtPushConstants), &pushConstants);

            vkCmdTraceRaysKHR(cmd, &mSbtRaygenRegion, &mSbtMissRegion, &mSbtHitRegion, &mSbtCallableRegion,
                rect.extent.width, rect.extent.height, 1);
        }

        ImageBarrier(cmd, mOutputImages[frame], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
}
