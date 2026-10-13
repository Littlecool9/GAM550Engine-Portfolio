/**
 * @file MeshUploader.cpp
 * @author Jackey Cen
 * @brief Records queued mesh uploads and their BLAS builds into frames, and frees retired meshes after their last frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MeshUploader.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

namespace mir::vk
{
    bool MeshUploader::Configure(const MeshStreamingDesc &desc)
    {
        if (desc.stagingBytes < MeshUploadQueue::kStagingAlignment || desc.frameBytes == 0)
        {
            MIR_LOG_ERROR("ConfigureMeshStreaming: {} staging bytes and {} bytes a frame are too few",
                desc.stagingBytes, desc.frameBytes);
            return false;
        }
        if (Configured() && desc == mQueue.Desc())
            return true;
        if (mQueue.Pending() > 0 || mQueue.Uploading() > 0)
        {
            MIR_LOG_ERROR("ConfigureMeshStreaming: uploads are queued or in flight; nothing changed");
            return false;
        }

        DestroyStaging();
        VmaAllocationInfo info{};
        if (!CreateBuffer(mDevice->Allocator(), desc.stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            mStaging, mStagingAllocation, &info))
        {
            MIR_LOG_CRITICAL("ConfigureMeshStreaming: staging ring allocation failed");
            mStaging = VK_NULL_HANDLE;
            mStagingAllocation = VK_NULL_HANDLE;
            return false;
        }
        mStagingMapped = static_cast<std::byte *>(info.pMappedData);
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mStaging), "Mesh staging ring");
        mQueue = MeshUploadQueue(desc);
        MIR_LOG_INFO("Mesh streaming: {} byte staging ring, {} bytes a frame", desc.stagingBytes, desc.frameBytes);
        return true;
    }

    void MeshUploader::DestroyStaging()
    {
        if (mStaging != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), mStaging, mStagingAllocation);
        mStaging = VK_NULL_HANDLE;
        mStagingAllocation = VK_NULL_HANDLE;
        mStagingMapped = nullptr;
    }

    void MeshUploader::Shutdown(RayTracer &rayTracer)
    {
        if (mDevice == nullptr)
            return;
        Clear(rayTracer);
        DestroyStaging();
    }

    uint64_t MeshUploader::Footprint(const RayTracer &rayTracer, uint32_t vertexCount, uint32_t indexCount,
        const MeshUploadOptions &options) const
    {
        uint64_t bytes = uint64_t(vertexCount) * sizeof(Vertex) + uint64_t(indexCount) * sizeof(uint32_t);
        if (options.rayTracing && mDevice->RayTracingSupported() && vertexCount > 0)
            bytes += rayTracer.MeshBlasSizes(vertexCount, indexCount).accelerationStructure;
        return bytes;
    }

    MeshHandle MeshUploader::Queue(ResourceStore &resources, const RayTracer &rayTracer, std::vector<Vertex> vertices,
        std::vector<uint32_t> indices, const MeshUploadOptions &options)
    {
        if (!Configured())
        {
            MIR_LOG_ERROR("QueueMesh: mesh streaming is not configured");
            return kNullMesh;
        }
        if (vertices.empty() || indices.empty())
        {
            MIR_LOG_ERROR("QueueMesh: vertices and indices must both be non-empty");
            return kNullMesh;
        }

        const bool blas = options.rayTracing && mDevice->RayTracingSupported();
        const uint64_t staging = vertices.size() * sizeof(Vertex) + indices.size() * sizeof(uint32_t);
        const uint64_t gpu = Footprint(rayTracer, uint32_t(vertices.size()), uint32_t(indices.size()), options);
        const MeshHandle handle = resources.ReserveMesh();
        resources.FindMeshSlot(handle)->mBytes = gpu;
        mQueue.Push({ handle, staging, gpu });
        mQueued[handle.mIndex] = Queued{ handle, std::move(vertices), std::move(indices), blas };
        ++mQueuedCount;
        return handle;
    }

    bool MeshUploader::Retire(ResourceStore &resources, RayTracer &rayTracer, MeshHandle handle, uint64_t lastUse)
    {
        GpuMesh *const mesh = resources.FindMeshSlot(handle);
        if (mesh == nullptr)
            return false;
        if (mesh->mState == GpuMeshState::Pending)
        {
            Forget(handle);
            resources.TakeMesh(handle);
            return true;
        }

        const GpuMesh taken = resources.TakeMesh(handle);
        Garbage garbage;
        if (mProfiling)
            garbage.retiredAt = std::chrono::steady_clock::now();
        garbage.blas = rayTracer.TakeMeshBlas(handle.mIndex);
        if (taken.mVertexBuffer != VK_NULL_HANDLE)
            garbage.buffers.emplace_back(taken.mVertexBuffer, taken.mVertexAllocation);
        if (taken.mIndexBuffer != VK_NULL_HANDLE)
            garbage.buffers.emplace_back(taken.mIndexBuffer, taken.mIndexAllocation);
        mRetired.Push(lastUse, taken.mState == GpuMeshState::Failed ? 0 : taken.mBytes, std::move(garbage));
        ++mRetiredCount;
        return true;
    }

    void MeshUploader::Forget(MeshHandle handle)
    {
        if (!mQueue.Cancel(handle))
            return;
        mQueued.erase(handle.mIndex);
        ++mCanceled;
    }

    void MeshUploader::Collect(ResourceStore &resources, RayTracer &rayTracer, uint64_t completed)
    {
        for (const MeshHandle handle : mQueue.Complete(completed))
        {
            GpuMesh *const mesh = resources.FindMeshSlot(handle);
            if (mesh == nullptr || mesh->mState != GpuMeshState::Uploading)
                continue;
            mesh->mState = GpuMeshState::Ready;
            ++mUploaded;
        }
        mRetired.Collect(completed, [&](Garbage &garbage) { Free(rayTracer, garbage); });
    }

    void MeshUploader::Record(VkCommandBuffer cmd, uint64_t frame, ResourceStore &resources, RayTracer &rayTracer)
    {
        if (!Configured())
            return;
        const std::vector<MeshUploadQueue::Admitted> admitted = mQueue.Admit(frame);
        mMaxFrameBytes = std::max(mMaxFrameBytes, mQueue.LastFrameBytes());
        if (admitted.empty())
            return;

        std::vector<RayTracer::MeshBlasBuild> builds;
        for (const MeshUploadQueue::Admitted &entry : admitted)
        {
            const MeshHandle handle = entry.item.mesh;
            const auto found = mQueued.find(handle.mIndex);
            GpuMesh *const mesh = resources.FindMeshSlot(handle);
            if (found == mQueued.end() || found->second.mesh != handle || mesh == nullptr ||
                mesh->mState != GpuMeshState::Pending)
                continue;
            const Queued queued = std::move(found->second);
            mQueued.erase(found);

            const VkDeviceSize vertexBytes = queued.vertices.size() * sizeof(Vertex);
            const VkDeviceSize indexBytes = queued.indices.size() * sizeof(uint32_t);
            if (!resources.CreateMeshBuffers(*mesh, handle.mIndex, vertexBytes, indexBytes, queued.blas))
            {
                MIR_LOG_ERROR("Mesh {}: device buffer allocation failed, the upload is dropped", handle.mIndex);
                mesh->mState = GpuMeshState::Failed;
                ++mFailed;
                continue;
            }

            VkBuffer source = mStaging;
            VkDeviceSize offset = 0;
            std::byte *target = nullptr;
            VmaAllocation targetAllocation = mStagingAllocation;
            if (entry.stagingOffset)
            {
                offset = *entry.stagingOffset;
                target = mStagingMapped + offset;
            }
            else
            {
                VmaAllocationInfo info{};
                VkBuffer own = VK_NULL_HANDLE;
                VmaAllocation ownAllocation = VK_NULL_HANDLE;
                if (!CreateBuffer(mDevice->Allocator(), vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                    own, ownAllocation, &info))
                {
                    MIR_LOG_ERROR("Mesh {}: staging buffer allocation failed, the upload is dropped", handle.mIndex);
                    resources.FreeMesh(*mesh);
                    mesh->mVertexBuffer = mesh->mIndexBuffer = VK_NULL_HANDLE;
                    mesh->mVertexAllocation = mesh->mIndexAllocation = VK_NULL_HANDLE;
                    mesh->mState = GpuMeshState::Failed;
                    ++mFailed;
                    continue;
                }
                source = own;
                target = static_cast<std::byte *>(info.pMappedData);
                targetAllocation = ownAllocation;
                mRetired.Push(frame, vertexBytes + indexBytes, Garbage{ {}, { { own, ownAllocation } } });
            }
            std::memcpy(target, queued.vertices.data(), vertexBytes);
            std::memcpy(target + vertexBytes, queued.indices.data(), indexBytes);
            vmaFlushAllocation(mDevice->Allocator(), targetAllocation, entry.stagingOffset ? offset : 0,
                vertexBytes + indexBytes);

            const VkBufferCopy vertexCopy{ .srcOffset = offset, .dstOffset = 0, .size = vertexBytes };
            vkCmdCopyBuffer(cmd, source, mesh->mVertexBuffer, 1, &vertexCopy);
            const VkBufferCopy indexCopy{ .srcOffset = offset + vertexBytes, .dstOffset = 0, .size = indexBytes };
            vkCmdCopyBuffer(cmd, source, mesh->mIndexBuffer, 1, &indexCopy);

            mesh->mIndexCount = uint32_t(queued.indices.size());
            mesh->mState = GpuMeshState::Uploading;
            mUploadedBytes += entry.item.gpuBytes;
            if (queued.blas)
                builds.push_back({ handle.mIndex, mesh, uint32_t(queued.vertices.size()) });
        }

        if (!builds.empty())
        {
            const RayTracer::BlasBatch batch = rayTracer.RecordMeshBlases(cmd, builds);
            mBlasBuilt += batch.built;
            mBlasBytes += batch.blasBytes;
            if (batch.scratchBuffer != VK_NULL_HANDLE)
                mRetired.Push(frame, batch.scratchBytes,
                    Garbage{ {}, { { batch.scratchBuffer, batch.scratchAllocation } } });
        }

        // Later frames draw and trace from these, in submission order on the one queue.
        VkPipelineStageFlags2 srcStages = VK_PIPELINE_STAGE_2_COPY_BIT;
        VkAccessFlags2 srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags2 dstStages = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        VkAccessFlags2 dstAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        if (mDevice->RayTracingSupported())
        {
            srcStages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            srcAccess |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            dstStages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            dstAccess |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
        }
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = srcStages,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStages,
            .dstAccessMask = dstAccess };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier };
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    void MeshUploader::Clear(RayTracer &rayTracer)
    {
        mQueue.Clear();
        mQueued.clear();
        mRetired.ReleaseAll([&](Garbage &garbage) { Free(rayTracer, garbage); });
    }

    void MeshUploader::Free(RayTracer &rayTracer, Garbage &garbage)
    {
        if (garbage.retiredAt)
            mRetirementTimings.push_back(std::chrono::duration<double>(
                std::chrono::steady_clock::now() - *garbage.retiredAt).count());
        if (garbage.blas.mBlas != VK_NULL_HANDLE || garbage.blas.mBuffer != VK_NULL_HANDLE)
            rayTracer.DestroyBLAS(garbage.blas);
        for (const auto &[buffer, allocation] : garbage.buffers)
            vmaDestroyBuffer(mDevice->Allocator(), buffer, allocation);
        garbage.buffers.clear();
    }

    void MeshUploader::FillStats(MeshStreamingStats &stats) const
    {
        stats.desc = mQueue.Desc();
        stats.pending = mQueue.Pending();
        stats.pendingBytes = mQueue.PendingBytes();
        stats.uploading = mQueue.Uploading();
        stats.uploadingBytes = mQueue.UploadingBytes();
        stats.retiring = mRetired.Size();
        stats.retiringBytes = mRetired.Bytes();
        stats.stagingUsedBytes = mQueue.StagingUsed();
        stats.queued = mQueuedCount;
        stats.uploaded = mUploaded;
        stats.canceled = mCanceled;
        stats.retired = mRetiredCount;
        stats.failed = mFailed;
        stats.uploadedBytes = mUploadedBytes;
        stats.blasBuilt = mBlasBuilt;
        stats.blasBytes = mBlasBytes;
        stats.lastFrameBytes = mQueue.LastFrameBytes();
        stats.maxFrameBytes = mMaxFrameBytes;
    }
}
