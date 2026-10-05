/**
 * @file VulkanTexture.cpp
 * @author Sumin Park
 * @brief Textures, mip generation, samplers and default textures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <renderer/VulkanRenderer.h>
#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        // Level 0, then every smaller level if desc asks for them, packed back
        // to back into one staging blob with a copy region each. Returns the
        // level count. The box filter averages in linear light, so an sRGB
        // image is decoded and re-encoded around the average; alpha is already
        // linear and is averaged as is.
        uint32_t BuildMipChain(const TextureDesc &desc, std::vector<std::byte> &bytes,
            std::vector<VkBufferImageCopy> &regions)
        {
            const auto toLinear = [](float v)
                {
                    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
                };
            const auto toSrgb = [](float v)
                {
                    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                };

            bytes.assign(desc.pixels.begin(), desc.pixels.end());
            std::vector<std::byte> level(desc.pixels.begin(), desc.pixels.end());

            uint32_t width = desc.width;
            uint32_t height = desc.height;
            uint32_t mip = 0;
            VkDeviceSize offset = 0;

            for (;;)
            {
                regions.push_back(VkBufferImageCopy{
                    .bufferOffset = offset,
                    .imageSubresource{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = mip,
                        .baseArrayLayer = 0,
                        .layerCount = 1 },
                    .imageExtent{ width, height, 1 } });

                if (!desc.generateMips || (width == 1 && height == 1))
                    break;

                const uint32_t nextWidth = std::max(width / 2, 1u);
                const uint32_t nextHeight = std::max(height / 2, 1u);
                std::vector<std::byte> next(static_cast<size_t>(nextWidth) * nextHeight * 4);

                for (uint32_t y = 0; y < nextHeight; ++y)
                    for (uint32_t x = 0; x < nextWidth; ++x)
                        for (uint32_t c = 0; c < 4; ++c)
                        {
                            float sum = 0.0f;
                            uint32_t samples = 0;
                            for (uint32_t sy = y * height / nextHeight; sy < (y + 1) * height / nextHeight; ++sy)
                                for (uint32_t sx = x * width / nextWidth; sx < (x + 1) * width / nextWidth; ++sx)
                                {
                                    const float v =
                                        std::to_integer<uint8_t>(level[(static_cast<size_t>(sy) * width + sx) * 4 + c]) / 255.0f;
                                    sum += (c < 3 && desc.srgb) ? toLinear(v) : v;
                                    ++samples;
                                }

                            float v = sum / static_cast<float>(samples);
                            if (c < 3 && desc.srgb)
                                v = toSrgb(v);
                            next[(static_cast<size_t>(y) * nextWidth + x) * 4 + c] =
                                static_cast<std::byte>(std::clamp<long>(std::lround(v * 255.0f), 0, 255));
                        }

                offset = bytes.size();
                bytes.insert(bytes.end(), next.begin(), next.end());
                level = std::move(next);
                width = nextWidth;
                height = nextHeight;
                ++mip;
            }

            return mip + 1;
        }
    }

    VkSampler VulkanRenderer::AcquireSampler(const SamplerDesc &desc)
    {
        const uint64_t key = (static_cast<uint64_t>(desc.magFilter) << 40) |
            (static_cast<uint64_t>(desc.minFilter) << 32) |
            (static_cast<uint64_t>(desc.mipmapMode) << 24) |
            (static_cast<uint64_t>(desc.addressModeU) << 16) |
            (static_cast<uint64_t>(desc.addressModeV) << 8) |
            (desc.useMips ? 1ull : 0ull);

        if (const auto it = mSamplers.find(key); it != mSamplers.end())
            return it->second;

        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = desc.magFilter,
            .minFilter = desc.minFilter,
            .mipmapMode = desc.mipmapMode,
            .addressModeU = desc.addressModeU,
            .addressModeV = desc.addressModeV,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .anisotropyEnable = VK_FALSE,
            .compareEnable = VK_FALSE,
            .minLod = 0.0f,
            .maxLod = desc.useMips ? VK_LOD_CLAMP_NONE : 0.0f,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK };

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(mDevice->Handle(), &info, nullptr, &sampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateSampler failed");
            return VK_NULL_HANDLE;
        }

        mSamplers.emplace(key, sampler);
        return sampler;
    }

    void VulkanRenderer::DestroySamplers()
    {
        for (const auto &[key, sampler] : mSamplers)
            vkDestroySampler(mDevice->Handle(), sampler, nullptr);
        mSamplers.clear();
    }

    bool VulkanRenderer::CreateDefaultTextures()
    {
        const std::array<std::byte, 4> white{
            std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };

        const std::array<std::byte, 4> flatNormal{
            std::byte{ 128 }, std::byte{ 128 }, std::byte{ 255 }, std::byte{ 255 } };

        mDefaultWhiteTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = white, .srgb = true });
        mDefaultNormalTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = flatNormal, .srgb = false });

        if (mDefaultWhiteTexture.IsNull() || mDefaultNormalTexture.IsNull())
        {
            MIR_LOG_CRITICAL("CreateDefaultTextures: default texture upload failed");
            return false;
        }

        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultWhiteTexture.mIndex].mImage), "Default white texture");
        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultNormalTexture.mIndex].mImage), "Default normal texture");

        return true;
    }

    TextureHandle VulkanRenderer::CreateTexture(const TextureDesc &desc)
    {
        const VkDeviceSize expectedBytes = VkDeviceSize{ desc.width } * desc.height * 4;
        if (desc.width == 0 || desc.height == 0 || desc.pixels.size_bytes() != expectedBytes)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} expects {} bytes of RGBA8, got {}",
                desc.width, desc.height, expectedBytes, desc.pixels.size_bytes());
            return kNullTexture;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(mDevice->Physical(), &properties);
        if (desc.width > properties.limits.maxImageDimension2D ||
            desc.height > properties.limits.maxImageDimension2D)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} is past this device's maxImageDimension2D of {}",
                desc.width, desc.height, properties.limits.maxImageDimension2D);
            return kNullTexture;
        }

        // One staging blob for every level, one copy region each.
        std::vector<std::byte> pixels;
        std::vector<VkBufferImageCopy> regions;
        const uint32_t levelCount = BuildMipChain(desc, pixels, regions);

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;

        if (!vk::CreateUploadBuffer(mDevice->Allocator(), pixels,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingAllocation))
        {
            MIR_LOG_CRITICAL("CreateTexture: staging buffer allocation failed");
            return kNullTexture;
        }

        GpuTexture texture{};
        texture.mLevels = levelCount;
        texture.mGeneration = 1;

        const VkFormat format = desc.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent{ desc.width, desc.height, 1 },
            .mipLevels = levelCount,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

        if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
            &texture.mImage, &texture.mAllocation, nullptr) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vmaCreateImage failed");
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullTexture;
        }

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = mDevice->QueueFamily() };

        if (vkCreateCommandPool(mDevice->Handle(), &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: upload command pool creation failed");
            vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullTexture;
        }

        VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo cmdAllocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = uploadPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        vkAllocateCommandBuffers(mDevice->Handle(), &cmdAllocInfo, &uploadCmd);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(uploadCmd, &beginInfo);

        vk::ImageBarrier(uploadCmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levelCount);

        vkCmdCopyBufferToImage(uploadCmd, staging, texture.mImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(regions.size()), regions.data());

        vk::ImageBarrier(uploadCmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, levelCount);

        vkEndCommandBuffer(uploadCmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &uploadCmd };

        vkQueueSubmit(mDevice->Queue(), 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(mDevice->Queue());

        vkDestroyCommandPool(mDevice->Handle(), uploadPool, nullptr);
        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.mImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = levelCount,
                .baseArrayLayer = 0,
                .layerCount = 1 } };

        if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &texture.mView) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vkCreateImageView failed");
            vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
            return kNullTexture;
        }

        const uint32_t index = static_cast<uint32_t>(mTextures.size());
        mTextures.push_back(texture);

        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture.mImage),
            std::format("Texture {}", index).c_str());

        MIR_LOG_INFO("Texture {} uploaded: {}x{}, {} level(s), {} bytes",
            index, desc.width, desc.height, levelCount, pixels.size());

        return TextureHandle{ index, texture.mGeneration };
    }

    const VulkanRenderer::GpuTexture *VulkanRenderer::FindTexture(TextureHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mTextures.size())
            return nullptr;

        const GpuTexture &texture = mTextures[handle.mIndex];

        if (texture.mGeneration != handle.mGeneration)
            return nullptr;

        return &texture;
    }

    void VulkanRenderer::DestroyTextures()
    {
        for (GpuTexture &texture : mTextures)
        {
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice->Handle(), texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
        }
        mTextures.clear();
        mDefaultWhiteTexture = kNullTexture;
        mDefaultNormalTexture = kNullTexture;
    }
}
