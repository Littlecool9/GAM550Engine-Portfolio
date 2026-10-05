/**
 * @file ModelDescriptors.cpp
 * @author Jackey Cen
 * @brief Per-draw model data (descriptor set 2): deformation buffers and their pools.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ModelDescriptors.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <utility>
#include <vector>

namespace mir::vk
{
    namespace
    {
        // Set 2 is allocated per draw and the pool is reset each frame, so
        // this is a ceiling on model draws in one frame, not a total.
        constexpr uint32_t kMaxModelDrawsPerFrame = 4096;
    } // namespace
    bool ModelDescriptors::Initialize(Device &device)
    {
        mDevice = &device;

        // set 2, binding 0: one model draw's deformation data, with the
        // material parameters in slot 2. The textures live in set 0 with every
        // other material's, so this set holds nothing but the buffer.
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = 1;
        info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &info, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout failed for the model set");
            return false;
        }
        // One set per model draw per frame; the whole pool is reset each frame.
        const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxModelDrawsPerFrame };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = kMaxModelDrawsPerFrame;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        for (auto &p : mPools)
            if (vkCreateDescriptorPool(mDevice->Handle(), &pool, nullptr, &p) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorPool failed for the model set");
                return false;
            }
        return true;
    }
    bool ModelDescriptors::Bind(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame,
        std::span<const glm::vec4> deformation, const glm::vec4 &materialParameters)
    {
        std::vector<glm::vec4> data(deformation.begin(), deformation.end());
        if (data.size() < 3)
            data.resize(3, glm::vec4(0));
        // Slot 2 is the material channel; BuildDeformationData leaves it free.
        data[2] = materialParameters;
        if (data.size() * sizeof(glm::vec4) > mDevice->Properties().limits.maxStorageBufferRange)
        {
            MIR_LOG_ERROR("Bind: model exceeds maxStorageBufferRange");
            return false;
        }
        FrameBuffer buffer{};
        if (!CreateUploadBuffer(mDevice->Allocator(), std::as_bytes(std::span(data)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buffer.buffer, buffer.allocation))
        {
            MIR_LOG_ERROR("Bind: model frame buffer allocation failed");
            return false;
        }
        mBuffers[frame].push_back(buffer);
        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = mPools[frame];
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &mSetLayout;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(mDevice->Handle(), &alloc, &set) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("Bind: model descriptor capacity exceeded ({} draws per frame)", kMaxModelDrawsPerFrame);
            return false;
        }
        VkDescriptorBufferInfo bufferInfo{ buffer.buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(mDevice->Handle(), 1, &write, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &set, 0, nullptr);
        return true;
    }
    void ModelDescriptors::ResetFrame(uint32_t frame)
    {
        for (auto b : mBuffers[frame])
            vmaDestroyBuffer(mDevice->Allocator(), b.buffer, b.allocation);
        mBuffers[frame].clear();
        if (mPools[frame] != VK_NULL_HANDLE)
            vkResetDescriptorPool(mDevice->Handle(), mPools[frame], 0);
    }

    void ModelDescriptors::ResetFrames()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
            ResetFrame(i);
    }
    void ModelDescriptors::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        ResetFrames();
        for (VkDescriptorPool &pool : mPools)
        {
            if (pool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice->Handle(), pool, nullptr);
                pool = VK_NULL_HANDLE;
            }
        }
        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }
    }
} // namespace mir::vk
