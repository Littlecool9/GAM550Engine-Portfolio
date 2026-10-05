/**
 * @file VulkanMesh.cpp
 * @author Sumin Park
 * @brief GPU mesh creation, lookup and destruction.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>
#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <cstring>
#include <format>

namespace mir
{
    MeshHandle VulkanRenderer::CreateMesh(std::span<const Vertex> vertices,
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

        if (!vk::CreateBuffer(mDevice->Allocator(), vertexBytes + indexBytes,
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
        mesh.mGeneration = 1;

        VkBufferUsageFlags extraUsage = 0;
        if (mDevice->RayTracingSupported())
            extraUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    // No host-access flags on either: that is what puts them in VRAM.
        const bool buffersCreated =
            vk::CreateBuffer(mDevice->Allocator(), vertexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage,
                0, mesh.mVertexBuffer, mesh.mVertexAllocation, nullptr) &&
            vk::CreateBuffer(mDevice->Allocator(), indexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage,
                0, mesh.mIndexBuffer, mesh.mIndexAllocation, nullptr);

        if (!buffersCreated)
        {
            MIR_LOG_CRITICAL("CreateMesh: device buffer allocation failed");

            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullMesh;
        }

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = mDevice->QueueFamily() };

        if (vkCreateCommandPool(mDevice->Handle(), &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateMesh: upload command pool creation failed");
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
            vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
            return kNullMesh;
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

        const VkBufferCopy vertexCopy{ .srcOffset = 0, .dstOffset = 0, .size = vertexBytes };
        vkCmdCopyBuffer(uploadCmd, staging, mesh.mVertexBuffer, 1, &vertexCopy);

        // srcOffset picks the second half of the same staging allocation.
        const VkBufferCopy indexCopy{ .srcOffset = vertexBytes, .dstOffset = 0, .size = indexBytes };
        vkCmdCopyBuffer(uploadCmd, staging, mesh.mIndexBuffer, 1, &indexCopy);

        VkBuffer blasScratch = VK_NULL_HANDLE;
        VmaAllocation blasScratchAllocation = VK_NULL_HANDLE;
        if (mDevice->RayTracingSupported())
        {
            if (!BuildBLAS(mesh, static_cast<uint32_t>(vertices.size()),
                static_cast<uint32_t>(indices.size()), uploadCmd, blasScratch, blasScratchAllocation))
                MIR_LOG_ERROR("CreateMesh: BLAS build failed, mesh will raster only");
        }

        vkEndCommandBuffer(uploadCmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &uploadCmd };

        vkQueueSubmit(mDevice->Queue(), 1, &submitInfo, VK_NULL_HANDLE);
        // wait till buffer is created
        vkQueueWaitIdle(mDevice->Queue());

        vkDestroyCommandPool(mDevice->Handle(), uploadPool, nullptr);
        vmaDestroyBuffer(mDevice->Allocator(), staging, stagingAllocation);
        if (blasScratch != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), blasScratch, blasScratchAllocation);

        uint32_t index;
        if (!mFreeMeshIndices.empty())
        {
            index = mFreeMeshIndices.back();
            mFreeMeshIndices.pop_back();
            // DestroyMesh already bumped the slot's generation; keep it.
            mesh.mGeneration = mMeshes[index].mGeneration;
            mMeshes[index] = mesh;
        }
        else
        {
            index = static_cast<uint32_t>(mMeshes.size());
            mMeshes.push_back(mesh);
        }

        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mVertexBuffer),
            std::format("Mesh {} vertices", index).c_str());
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mIndexBuffer),
            std::format("Mesh {} indices", index).c_str());

        MIR_LOG_INFO("Mesh {} uploaded: {} vertices, {} indices, {} bytes",
            index, vertices.size(), indices.size(), vertexBytes + indexBytes);

        return MeshHandle{ index, mesh.mGeneration };
    }

    bool VulkanRenderer::DestroyMesh(MeshHandle handle)
    {
        if (handle.IsNull() || handle.mIndex >= mMeshes.size() ||
            mMeshes[handle.mIndex].mGeneration != handle.mGeneration)
        {
            MIR_LOG_ERROR("DestroyMesh: handle is null or stale");
            return false;
        }

        // in-flight frames (up to kFramesInFlight - 1 old ones) may still be
        // drawing from this buffer - see ReleaseSceneResources.
        vkDeviceWaitIdle(mDevice->Handle());

        GpuMesh &mesh = mMeshes[handle.mIndex];
        DestroyBLAS(mesh);
        if (mesh.mVertexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
        if (mesh.mIndexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);

        const uint32_t nextGeneration = mesh.mGeneration + 1;
        mesh = GpuMesh{};
        mesh.mGeneration = nextGeneration; // every outstanding copy of handle is now stale

        mFreeMeshIndices.push_back(handle.mIndex);
        return true;
    }

    const VulkanRenderer::GpuMesh *VulkanRenderer::FindMesh(MeshHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mMeshes.size())
            return nullptr;

        const GpuMesh &mesh = mMeshes[handle.mIndex];

        if (mesh.mGeneration != handle.mGeneration)
            return nullptr;

        return &mesh;
    }

    void VulkanRenderer::DestroyMeshes()
    {
        for (GpuMesh &mesh : mMeshes)
        {
            DestroyBLAS(mesh);
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
        }
        mMeshes.clear();
        mFreeMeshIndices.clear();
    }
}
