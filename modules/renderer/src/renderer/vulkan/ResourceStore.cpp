/**
 * @file ResourceStore.cpp
 * @author Sumin Park
 * @brief Mesh and texture uploads, mip generation, samplers and default textures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ResourceStore.h"
#include "Device.h"
#include "Slots.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace mir::vk
{
    namespace
    {
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

    bool ResourceStore::Initialize(Device &device)
    {
        mDevice = &device;
        return CreateDefaultTextures();
    }

    void ResourceStore::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        ReleaseMeshes();
        DestroyTextures();
        DestroySamplers();
    }

    MeshHandle ResourceStore::CreateMesh(std::span<const Vertex> vertices,
        std::span<const uint32_t> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            MIR_LOG_ERROR("CreateMesh: vertices and indices must both be non-empty");
            return kNullMesh;
        }

        const VkDeviceSize vertexBytes = vertices.size_bytes();
        const VkDeviceSize indexBytes = indices.size_bytes();

        // temporarily pack multiple data to one buffer for faster creation
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingInfo{};

        if (!CreateBuffer(mDevice->Allocator(), vertexBytes + indexBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            staging, stagingAllocation, &stagingInfo))
        {
            MIR_LOG_CRITICAL("CreateMesh: staging buffer allocation failed");
            return kNullMesh;
        }

        std::byte *const mapped = static_cast<std::byte *>(stagingInfo.pMappedData);
        std::memcpy(mapped, vertices.data(), vertexBytes);
        std::memcpy(mapped + vertexBytes, indices.data(), indexBytes);
        vmaFlushAllocation(mDevice->Allocator(), stagingAllocation, 0, VK_WHOLE_SIZE);

        GpuMesh mesh{};
        mesh.mIndexCount = static_cast<uint32_t>(indices.size());

        // Ray tracing reads both buffers through device addresses to build the BLAS.
        VkBufferUsageFlags extraUsage = 0;
        if (mDevice->RayTracingSupported())
            extraUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

        // No host-access flags on either: that is what puts them in VRAM.
        const bool buffersCreated =
            CreateBuffer(mDevice->Allocator(), vertexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage,
                0, mesh.mVertexBuffer, mesh.mVertexAllocation, nullptr) &&
            CreateBuffer(mDevice->Allocator(), indexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage,
                0, mesh.mIndexBuffer, mesh.mIndexAllocation, nullptr);

        if (!buffersCreated)
        {
            MIR_LOG_CRITICAL("CreateMesh: device buffer allocation failed");
            FreeMesh(mesh);
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullMesh;
        }

        const bool uploaded = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                const VkBufferCopy vertexCopy{ .srcOffset = 0, .dstOffset = 0, .size = vertexBytes };
                vkCmdCopyBuffer(cmd, staging, mesh.mVertexBuffer, 1, &vertexCopy);

                // srcOffset picks the second half of the same staging allocation.
                const VkBufferCopy indexCopy{ .srcOffset = vertexBytes, .dstOffset = 0, .size = indexBytes };
                vkCmdCopyBuffer(cmd, staging, mesh.mIndexBuffer, 1, &indexCopy);
            });

        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);

        if (!uploaded)
        {
            FreeMesh(mesh);
            return kNullMesh;
        }

        const uint32_t index = PlaceSlot(mMeshes, mFreeMeshIndices, mesh);

        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mVertexBuffer),
            std::format("Mesh {} vertices", index).c_str());
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mIndexBuffer),
            std::format("Mesh {} indices", index).c_str());

        MIR_LOG_INFO("Mesh {} uploaded: {} vertices, {} indices, {} bytes",
            index, vertices.size(), indices.size(), vertexBytes + indexBytes);

        return MeshHandle{ index, mMeshes[index].mGeneration };
    }

    void ResourceStore::FreeMesh(GpuMesh &mesh)
    {
        if (mesh.mVertexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
        if (mesh.mIndexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
    }

    bool ResourceStore::DestroyMesh(MeshHandle handle)
    {
        if (FindMesh(handle) == nullptr)
            return false;

        GpuMesh &mesh = mMeshes[handle.mIndex];
        FreeMesh(mesh);
        RetireSlot(mesh);

        mFreeMeshIndices.push_back(handle.mIndex);
        return true;
    }

    std::size_t ResourceStore::ReleaseMeshes()
    {
        const std::size_t count = mMeshes.size();
        for (GpuMesh &mesh : mMeshes)
            FreeMesh(mesh);
        mMeshes.clear();
        mFreeMeshIndices.clear();
        return count;
    }

    const GpuMesh *ResourceStore::FindMesh(MeshHandle handle) const
    {
        return FindSlot(mMeshes, handle);
    }

    VkSampler ResourceStore::AcquireSampler(const SamplerDesc &desc)
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
            MIR_LOG_CRITICAL("AcquireSampler: vkCreateSampler failed");
            return VK_NULL_HANDLE;
        }

        mSamplers.emplace(key, sampler);
        return sampler;
    }

    void ResourceStore::DestroySamplers()
    {
        for (const auto &[key, sampler] : mSamplers)
            vkDestroySampler(mDevice->Handle(), sampler, nullptr);
        mSamplers.clear();
    }

    bool ResourceStore::CreateDefaultTextures()
    {
        const std::array<std::byte, 4> white{
            std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };

        const std::array<std::byte, 4> flatNormal{
            std::byte{ 128 }, std::byte{ 128 }, std::byte{ 255 }, std::byte{ 255 } };

        mDefaultWhiteTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = white, .srgb = true });
        mDefaultNormalTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = flatNormal, .srgb = false });

        constexpr uint32_t checkerSize = 32;
        constexpr uint32_t cell = 16;

        std::vector<std::byte> checker(static_cast<size_t>(checkerSize) * checkerSize * 4);
        for (int y = 0; y < checkerSize; ++y)
        {
            for (int x = 0; x < checkerSize; ++x)
            {
                // start with black
                bool magenta = (x / cell + y / cell) % 2;
                size_t i = (static_cast<size_t>(y) * checkerSize + x) * 4;
                checker[i + 0] = magenta ? std::byte{ 255 } : std::byte{ 0 };
                checker[i + 1] = std::byte{ 0 };
                checker[i + 2] = magenta ? std::byte{ 255 } : std::byte{ 0 };
                checker[i + 3] = std::byte{ 255 };
            }
        }

        mErrorTexture = CreateTexture(TextureDesc{
            .width = checkerSize, .height = checkerSize, .pixels = checker, .srgb = true
            });

        if (mDefaultWhiteTexture.IsNull() || mDefaultNormalTexture.IsNull() || mErrorTexture.IsNull())
        {
            MIR_LOG_CRITICAL("CreateDefaultTextures: default texture upload failed");
            return false;
        }

        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultWhiteTexture.mIndex].mImage), "Default white texture");
        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultNormalTexture.mIndex].mImage), "Default normal texture");
        mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mErrorTexture.mIndex].mImage), "Error Texture");

        return true;
    }

    TextureHandle ResourceStore::CreateTexture(const TextureDesc &desc)
    {
        const VkDeviceSize expectedBytes = VkDeviceSize{ desc.width } * desc.height * 4;
        if (desc.width == 0 || desc.height == 0 || desc.pixels.size_bytes() != expectedBytes)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} expects {} bytes of RGBA8, got {}",
                desc.width, desc.height, expectedBytes, desc.pixels.size_bytes());
            return kNullTexture;
        }

        const uint32_t maxDimension = mDevice->Properties().limits.maxImageDimension2D;
        if (desc.width > maxDimension || desc.height > maxDimension)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} is past this device's maxImageDimension2D of {}",
                desc.width, desc.height, maxDimension);
            return kNullTexture;
        }

        // One staging blob for every level, one copy region each.
        std::vector<std::byte> pixels;
        std::vector<VkBufferImageCopy> regions;
        const uint32_t levelCount = BuildMipChain(desc, pixels, regions);

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;

        if (!CreateUploadBuffer(mDevice->Allocator(), pixels,
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

        const bool uploaded = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                ImageBarrier(cmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levelCount);

                vkCmdCopyBufferToImage(cmd, staging, texture.mImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<uint32_t>(regions.size()), regions.data());

                ImageBarrier(cmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, levelCount);
            });

        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);

        if (!uploaded)
        {
            FreeTexture(texture);
            return kNullTexture;
        }

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
            FreeTexture(texture);
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

    void ResourceStore::FreeTexture(GpuTexture &texture)
    {
        if (texture.mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice->Handle(), texture.mView, nullptr);
        if (texture.mImage != VK_NULL_HANDLE)
            vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
    }

    bool ResourceStore::DestroyTexture(TextureHandle handle)
    {
        if (handle == mDefaultWhiteTexture || handle == mDefaultNormalTexture || handle == mErrorTexture || FindTexture(handle) == nullptr)
            return false;

        GpuTexture &texture = mTextures[handle.mIndex];
        FreeTexture(texture);
        RetireSlot(texture);
        return true;
    }

    std::size_t ResourceStore::ReleaseTextures()
    {
        // defaults hold the first few slots
        // update slot if new default texture added
        const std::size_t kept = std::min<std::size_t>(mTextures.size(), 3);
        for (std::size_t i = kept; i < mTextures.size(); ++i)
            FreeTexture(mTextures[i]);
        const std::size_t released = mTextures.size() - kept;
        mTextures.resize(kept);
        return released;
    }

    const GpuTexture *ResourceStore::FindTexture(TextureHandle handle) const
    {
        return FindSlot(mTextures, handle);
    }

    void ResourceStore::DestroyTextures()
    {
        for (GpuTexture &texture : mTextures)
            FreeTexture(texture);
        mTextures.clear();
        mDefaultWhiteTexture = kNullTexture;
        mDefaultNormalTexture = kNullTexture;
    }
}
