/**
 * @file MeshUploader.h
 * @author Jackey Cen
 * @brief Records queued mesh uploads and their BLAS builds into frames, and frees retired meshes after their last frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "RayTracer.h"
#include "ResourceStore.h"
#include <renderer/Mesh.h>
#include <renderer/MeshStreaming.h>

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    // Owns the persistent staging ring. Nothing here waits: a frame's uploads are recorded at its start and are
    // ready once the frame timeline passes that frame.
    class MeshUploader
    {
    public:
        void Initialize(Device &device) { mDevice = &device; }
        // Creates the staging ring; a later, different desc needs nothing queued or in flight.
        bool Configure(const MeshStreamingDesc &desc);
        bool Configured() const { return mStaging != VK_NULL_HANDLE; }
        // With the device idle.
        void Shutdown(RayTracer &rayTracer);

        MeshHandle Queue(ResourceStore &resources, const RayTracer &rayTracer, std::vector<Vertex> vertices,
            std::vector<uint32_t> indices, const MeshUploadOptions &options);
        uint64_t Footprint(const RayTracer &rayTracer, uint32_t vertexCount, uint32_t indexCount,
            const MeshUploadOptions &options) const;
        // Freed once lastUse completes; a mesh not recorded yet is canceled at once. False for a stale handle.
        bool Retire(ResourceStore &resources, RayTracer &rayTracer, MeshHandle mesh, uint64_t lastUse);
        // Drops a mesh the caller destroys itself, with the device idle.
        void Forget(MeshHandle mesh);
        // Uploads of completed frames become Ready and retired objects are freed.
        void Collect(ResourceStore &resources, RayTracer &rayTracer, uint64_t completed);
        // The copies and BLAS builds of what this frame's budget admits, before anything draws.
        void Record(VkCommandBuffer cmd, uint64_t frame, ResourceStore &resources, RayTracer &rayTracer);
        // Every queued or in-flight upload and every retired object, with the device idle.
        void Clear(RayTracer &rayTracer);

        void FillStats(MeshStreamingStats &stats) const;

    private:
        struct Queued
        {
            MeshHandle mesh;
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            bool blas = false;
        };

        struct Garbage
        {
            RayTracer::MeshBlas blas;
            std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
        };

        void Free(RayTracer &rayTracer, Garbage &garbage);
        void DestroyStaging();

        Device *mDevice = nullptr;
        MeshUploadQueue mQueue;
        VkBuffer mStaging = VK_NULL_HANDLE;
        VmaAllocation mStagingAllocation = VK_NULL_HANDLE;
        std::byte *mStagingMapped = nullptr;
        // By mesh slot.
        std::unordered_map<uint32_t, Queued> mQueued;
        FrameRetireList<Garbage> mRetired;

        uint64_t mQueuedCount = 0;
        uint64_t mUploaded = 0;
        uint64_t mCanceled = 0;
        uint64_t mRetiredCount = 0;
        uint64_t mFailed = 0;
        uint64_t mUploadedBytes = 0;
        uint64_t mBlasBuilt = 0;
        uint64_t mBlasBytes = 0;
        uint64_t mMaxFrameBytes = 0;
    };
}
