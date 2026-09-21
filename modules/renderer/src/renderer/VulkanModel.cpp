#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <core/log/Log.h>
#include <cstring>
#include <utility>
#include <vector>
#include <renderer/VulkanRenderer.h>
#include <vk_mem_alloc.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace mir
{
    namespace
    {
        bool UploadBuffer(VmaAllocator allocator, std::span<const std::byte> bytes, VkBufferUsageFlags usage,
                          VkBuffer &buffer, VmaAllocation &allocation)
        {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes.size();
            info.usage = usage;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped{};
            if (vmaCreateBuffer(allocator, &info, &alloc, &buffer, &allocation, &mapped) != VK_SUCCESS)
                return false;
            std::memcpy(mapped.pMappedData, bytes.data(), bytes.size());
            vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
            return true;
        }
        // Set 2 is allocated per draw and the pool is reset each frame, so
        // this is a ceiling on model draws in one frame, not a total.
        constexpr uint32_t kMaxModelDrawsPerFrame = 4096;
    } // namespace
    bool VulkanRenderer::CreateModelDescriptors()
    {
        // set 2, binding 0: one model draw's deformation data, with the
        // material parameters in slot 2. The textures live in set 0 with every
        // other material's, so this set holds nothing but the buffer.
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(mDevice, &info, nullptr, &mModelSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDescriptorSetLayout failed for the model set");
            return false;
        }
        // One set per model draw per frame; the whole pool is reset each frame.
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxModelDrawsPerFrame};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = kMaxModelDrawsPerFrame;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        for (auto &p : mModelPools)
            if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &p) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateDescriptorPool failed for the model set");
                return false;
            }
        return true;
    }
    bool VulkanRenderer::BindModelDescriptors(VkCommandBuffer cmd, const GpuMaterial &material, const DrawItem &item)
    {
        std::vector<glm::vec4> data(item.deformation.begin(), item.deformation.end());
        if (data.size() < 3)
            data.resize(3, glm::vec4(0));
        // Slot 2 is the material channel; BuildDeformationData leaves it free.
        data[2] = material.mParameters;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        if (data.size() * sizeof(glm::vec4) > props.limits.maxStorageBufferRange)
        {
            MIR_LOG_ERROR("model exceeds maxStorageBufferRange");
            return false;
        }
        ModelFrameBuffer buffer{};
        if (!UploadBuffer(mAllocator, std::as_bytes(std::span(data)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buffer.buffer,
                          buffer.allocation))
        {
            MIR_LOG_ERROR("model frame buffer allocation failed");
            return false;
        }
        mModelBuffers[mFrameIndex].push_back(buffer);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = mModelPools[mFrameIndex];
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &mModelSetLayout;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(mDevice, &alloc, &set) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("model descriptor capacity exceeded ({} draws per frame)", kMaxModelDrawsPerFrame);
            return false;
        }
        VkDescriptorBufferInfo bufferInfo{buffer.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 2, 1, &set, 0, nullptr);
        return true;
    }
    void VulkanRenderer::ResetModelFrame(uint32_t frame)
    {
        for (auto b : mModelBuffers[frame])
            vmaDestroyBuffer(mAllocator, b.buffer, b.allocation);
        mModelBuffers[frame].clear();
        if (mModelPools[frame])
            vkResetDescriptorPool(mDevice, mModelPools[frame], 0);
    }
    void VulkanRenderer::DestroyModelResources(bool shutdown)
    {
        if (mCaptureBuffer)
        {
            vmaDestroyBuffer(mAllocator, mCaptureBuffer, mCaptureAllocation);
            mCaptureBuffer = VK_NULL_HANDLE;
            mCaptureAllocation = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            ResetModelFrame(i);
            if (shutdown && mModelPools[i])
            {
                vkDestroyDescriptorPool(mDevice, mModelPools[i], nullptr);
                mModelPools[i] = VK_NULL_HANDLE;
            }
        }
        if (shutdown && mModelSetLayout)
        {
            vkDestroyDescriptorSetLayout(mDevice, mModelSetLayout, nullptr);
            mModelSetLayout = VK_NULL_HANDLE;
        }
    }
    bool VulkanRenderer::RecordCapture(VkCommandBuffer cmd, uint32_t imageIndex)
    {
        if (mCapturePath.empty())
            return false;
        if (!mCanCapture ||
            (mSwapchainFormat != VK_FORMAT_B8G8R8A8_SRGB && mSwapchainFormat != VK_FORMAT_R8G8B8A8_SRGB))
        {
            MIR_LOG_ERROR("frame capture unavailable for this surface format");
            mCapturePath.clear();
            return false;
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = uint64_t(mSwapchainExtent.width) * mSwapchainExtent.height * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(mAllocator, &info, &allocation, &mCaptureBuffer, &mCaptureAllocation, nullptr) !=
            VK_SUCCESS)
        {
            MIR_LOG_ERROR("capture allocation failed");
            mCapturePath.clear();
            return false;
        }
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = mSwapchainImages[imageIndex];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {mSwapchainExtent.width, mSwapchainExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, mSwapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mCaptureBuffer,
                               1, &copy);
        VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = mCaptureBuffer;
        host.size = VK_WHOLE_SIZE;
        dep.imageMemoryBarrierCount = 0;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd, &dep);
        return true;
    }
    void VulkanRenderer::FinishCapture()
    {
        vkQueueWaitIdle(mGfxQueue);
        vmaInvalidateAllocation(mAllocator, mCaptureAllocation, 0, VK_WHOLE_SIZE);
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(mAllocator, mCaptureAllocation, &info);
        const auto size = size_t(mSwapchainExtent.width) * mSwapchainExtent.height * 4;
        const auto *begin = static_cast<const uint8_t *>(info.pMappedData);
        std::vector<uint8_t> pixels(begin, begin + size);
        if (mSwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < size; i += 4)
                std::swap(pixels[i], pixels[i + 2]);
        if (!stbi_write_png(mCapturePath.string().c_str(), int(mSwapchainExtent.width), int(mSwapchainExtent.height), 4,
                            pixels.data(), int(mSwapchainExtent.width * 4)))
            MIR_LOG_ERROR("cannot write frame capture: {}", mCapturePath.string());
        else
            MIR_LOG_INFO("Captured frame: {}", mCapturePath.string());
        vmaDestroyBuffer(mAllocator, mCaptureBuffer, mCaptureAllocation);
        mCaptureBuffer = VK_NULL_HANDLE;
        mCaptureAllocation = VK_NULL_HANDLE;
        mCapturePath.clear();
    }
} // namespace mir
