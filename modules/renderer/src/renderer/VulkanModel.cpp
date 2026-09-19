#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <core/log/Log.h>
#include <cstring>
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
        void TextureBarrier(VkCommandBuffer cmd, VkImage image, uint32_t levels, bool ready)
        {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = ready ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.srcAccessMask = ready ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0;
            b.dstStageMask = ready ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = ready ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = ready ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    } // namespace
    bool VulkanRenderer::CreateModelDescriptors()
    {
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr}};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 3;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(mDevice, &info, nullptr, &mModelSetLayout) != VK_SUCCESS)
            return false;
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 4096},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 4096;
        pool.poolSizeCount = 3;
        pool.pPoolSizes = sizes;
        for (auto &p : mModelPools)
            if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &p) != VK_SUCCESS)
                return false;
        const std::array<uint8_t, 4> white{255, 255, 255, 255};
        return CreateTexture(1, 1, white) != UINT32_MAX;
    }
    uint32_t VulkanRenderer::CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
    {
        if (width == 0 || height == 0 || uint64_t(width) * height * 4 != rgba.size())
            return UINT32_MAX;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        if (width > props.limits.maxImageDimension2D || height > props.limits.maxImageDimension2D)
            return UINT32_MAX;
        // Generate mip levels in linear light; upload bytes into an sRGB image.
        std::vector<uint8_t> bytes(rgba.begin(), rgba.end()), level = bytes;
        std::vector<VkBufferImageCopy> regions;
        uint32_t w = width, h = height, mip = 0;
        size_t offset = 0;
        auto linear = [](float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); };
        auto srgb = [](float v) { return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - .055f; };
        for (;;)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            region.imageExtent = {w, h, 1};
            regions.push_back(region);
            if (w == 1 && h == 1)
                break;
            const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
            std::vector<uint8_t> next(size_t(nw) * nh * 4);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        float sum = 0;
                        uint32_t samples = 0;
                        for (uint32_t sy = y * h / nh; sy < (y + 1) * h / nh; ++sy)
                            for (uint32_t sx = x * w / nw; sx < (x + 1) * w / nw; ++sx)
                            {
                                float v = level[(size_t(sy) * w + sx) * 4 + c] / 255.0f;
                                sum += c < 3 ? linear(v) : v;
                                ++samples;
                            }
                        float v = sum / samples;
                        if (c < 3)
                            v = srgb(v);
                        next[(size_t(y) * nw + x) * 4 + c] = uint8_t(std::clamp(std::lround(v * 255), 0l, 255l));
                    }
            offset = bytes.size();
            bytes.insert(bytes.end(), next.begin(), next.end());
            level = std::move(next);
            w = nw;
            h = nh;
            ++mip;
        }
        GpuTexture tex;
        tex.levels = mip + 1;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        auto cleanup = [&]() {
            if (pool)
                vkDestroyCommandPool(mDevice, pool, nullptr);
            if (staging)
                vmaDestroyBuffer(mAllocator, staging, stagingAlloc);
        };
        auto fail = [&]() {
            cleanup();
            if (tex.view)
                vkDestroyImageView(mDevice, tex.view, nullptr);
            if (tex.image)
                vmaDestroyImage(mAllocator, tex.image, tex.allocation);
            MIR_LOG_ERROR("model texture upload failed");
            return UINT32_MAX;
        };
        if (!UploadBuffer(mAllocator, std::as_bytes(std::span(bytes)), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging,
                          stagingAlloc))
            return fail();
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_SRGB;
        image.extent = {width, height, 1};
        image.mipLevels = tex.levels;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(mAllocator, &image, &alloc, &tex.image, &tex.allocation, nullptr) != VK_SUCCESS)
            return fail();
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = tex.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex.levels, 0, 1};
        if (vkCreateImageView(mDevice, &view, nullptr, &tex.view) != VK_SUCCESS)
            return fail();
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = mGfxQueueFamIdx;
        if (vkCreateCommandPool(mDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            return fail();
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(mDevice, &ci, &cmd) != VK_SUCCESS)
            return fail();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
            return fail();
        TextureBarrier(cmd, tex.image, tex.levels, false);
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                               regions.data());
        TextureBarrier(cmd, tex.image, tex.levels, true);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
            return fail();
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (vkQueueSubmit(mGfxQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
            return fail();
        vkQueueWaitIdle(mGfxQueue);
        cleanup();
        mTextures.push_back(tex);
        return uint32_t(mTextures.size() - 1);
    }
    bool VulkanRenderer::CreateMaterialSampler(GpuMaterial &material, const MaterialDesc &desc)
    {
        material.modelShader = desc.shaderName == "model";
        if (!material.modelShader)
            return true;
        material.texture = desc.texture == UINT32_MAX ? 0 : desc.texture;
        if (material.texture >= mTextures.size())
            return false;
        material.parameters = {float(desc.alphaMode), desc.alphaCutoff,
                               desc.cullMode == VK_CULL_MODE_NONE ? 1.0f : 0.0f, 0};
        auto wrap = [](uint32_t v) {
            return v == 33071   ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                   : v == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = desc.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter = (desc.minFilter == 9728 || desc.minFilter == 9984 || desc.minFilter == 9986)
                             ? VK_FILTER_NEAREST
                             : VK_FILTER_LINEAR;
        info.mipmapMode = (desc.minFilter == 9986 || desc.minFilter == 9987) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = wrap(desc.wrapS);
        info.addressModeV = wrap(desc.wrapT);
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxLod = desc.minFilter >= 9984 ? float(mTextures[material.texture].levels - 1) : 0;
        return vkCreateSampler(mDevice, &info, nullptr, &material.sampler) == VK_SUCCESS;
    }
    bool VulkanRenderer::BindModelDescriptors(VkCommandBuffer cmd, const GpuMaterial &material, const DrawItem &item)
    {
        std::vector<glm::vec4> data(item.deformation.begin(), item.deformation.end());
        if (data.size() < 3)
            data.resize(3, glm::vec4(0));
        data[2] = material.parameters;
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
            MIR_LOG_ERROR("model descriptor capacity exceeded (4096 draws per frame)");
            return false;
        }
        VkDescriptorImageInfo image{VK_NULL_HANDLE, mTextures[material.texture].view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sampler{material.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorBufferInfo bi{buffer.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &bi;
        vkUpdateDescriptorSets(mDevice, 3, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &set, 0, nullptr);
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
        for (size_t i = shutdown ? 0 : 1; i < mTextures.size(); ++i)
        {
            vkDestroyImageView(mDevice, mTextures[i].view, nullptr);
            vmaDestroyImage(mAllocator, mTextures[i].image, mTextures[i].allocation);
        }
        mTextures.resize(shutdown ? 0 : std::min(size_t(1), mTextures.size()));
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
