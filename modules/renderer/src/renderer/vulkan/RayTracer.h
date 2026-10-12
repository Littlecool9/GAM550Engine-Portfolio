/**
 * @file RayTracer.h
 * @author Sumin Park
 * @brief Ray tracing: per-mesh BLAS, per-frame TLAS, RT pipeline, SBT and output images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/VulkanRenderer.h>
#include "VulkanConstants.h"

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;
    class MaterialStore;
    struct GpuMesh;

    // Only initialized when the device supports ray tracing.
    class RayTracer
    {
    public:
        // The output images match `extent`; the camera set is borrowed as set 1 of the pipeline layout.
        bool Initialize(Device &device, VkExtent2D extent, VkDescriptorSetLayout cameraSetLayout);
        void Shutdown();

        // Recreates the output images at the swapchain's new extent; the caller idles the device first.
        bool Resize(VkExtent2D extent);
        bool PrepareDiagnostics();
        VkImage DiagnosticImage(uint32_t frame) const { return mDiagnosticImages[frame]; }

        struct MeshBlas
        {
            VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
            VkBuffer mBuffer = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkDeviceAddress mAddress = 0;
        };

        struct BlasSizes
        {
            VkDeviceSize accelerationStructure = 0;
            VkDeviceSize scratch = 0;
        };

        // Indexed by mesh slot, so the resource store never sees ray tracing. False leaves the mesh raster-only.
        bool BuildMeshBlas(uint32_t meshIndex, const GpuMesh &mesh, uint32_t vertexCount);
        struct MeshBlasBuild
        {
            uint32_t meshIndex = 0;
            const GpuMesh *mesh = nullptr;
            uint32_t vertexCount = 0;
        };

        struct BlasBatch
        {
            size_t built = 0;
            VkDeviceSize blasBytes = 0;
            // Shared by the batch; it must outlive the frame.
            VkBuffer scratchBuffer = VK_NULL_HANDLE;
            VmaAllocation scratchAllocation = VK_NULL_HANDLE;
            VkDeviceSize scratchBytes = 0;
        };

        // One copy-to-build barrier, then every build in one call; a mesh whose BLAS fails rasters only.
        BlasBatch RecordMeshBlases(VkCommandBuffer cmd, std::span<const MeshBlasBuild> builds);
        BlasSizes MeshBlasSizes(uint32_t vertexCount, uint32_t indexCount) const;
        // No-op for a slot that never had a BLAS.
        void DestroyMeshBlas(uint32_t meshIndex);
        // Moves the slot's BLAS out for a later DestroyBLAS; empty when it has none.
        MeshBlas TakeMeshBlas(uint32_t meshIndex);
        void DestroyBLAS(MeshBlas &blas);
        void DestroyBlases();

        // Builds the frame's TLAS from `items` and traces `rect` into the output image,
        // which ends in TRANSFER_SRC for the caller to blit.
        void Record(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items, const VkRect2D &rect,
            VkDescriptorSet cameraSet, uint32_t cameraOffset, const ResourceStore &resources,
            const MaterialStore &materials, bool diagnostics = false);

        VkImage OutputImage(uint32_t frame) const { return mOutputImages[frame]; }

    private:
        bool CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
            VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS);
        bool CreateOutputImages(VkExtent2D extent);
        void DestroyOutputImages();
        bool CreateScene();
        void DestroyScene();
        bool CreatePipeline(VkDescriptorSetLayout cameraSetLayout);
        void WriteDescriptorSets();
        bool BuildBLAS(const GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
            VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation);
        // Aligned to minAccelerationStructureScratchOffsetAlignment.
        bool CreateScratchBuffer(VkDeviceSize size, VkBuffer &outBuffer, VmaAllocation &outAllocation);
        void RecordTlasBuild(VkCommandBuffer cmd, uint32_t frame, std::span<const DrawItem> items,
            const ResourceStore &resources, const MaterialStore &materials);

        Device *mDevice = nullptr;
        VkDeviceSize mScratchAlignment = 1;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        VkBuffer mSbtBuffer = VK_NULL_HANDLE;
        VmaAllocation mSbtAllocation = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR mSbtRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mSbtMissRegion{};
        VkStridedDeviceAddressRegionKHR mSbtHitRegion{};
        VkStridedDeviceAddressRegionKHR mSbtCallableRegion{};

        constexpr static uint32_t kMaxInstances = 16384;
        bool mWarnedInstanceCap = false;

        VkImage mOutputImages[kFramesInFlight]{};
        VmaAllocation mOutputAllocations[kFramesInFlight]{};
        VkImageView mOutputViews[kFramesInFlight]{};
        VkExtent2D mExtent{};
        VkImage mDiagnosticImages[kFramesInFlight]{};
        VmaAllocation mDiagnosticAllocations[kFramesInFlight]{};
        VkImageView mDiagnosticViews[kFramesInFlight]{};

        VkAccelerationStructureKHR mTlas[kFramesInFlight]{};
        VkBuffer mTlasBuffers[kFramesInFlight]{};
        VmaAllocation mTlasAllocations[kFramesInFlight]{};

        VkBuffer mTlasScratchBuffers[kFramesInFlight]{};
        VmaAllocation mTlasScratchAllocations[kFramesInFlight]{};
        VkDeviceAddress mTlasScratchAddresses[kFramesInFlight]{};

        VkBuffer mInstanceBuffers[kFramesInFlight]{};
        VmaAllocation mInstanceAllocations[kFramesInFlight]{};
        std::byte *mInstanceMapped[kFramesInFlight]{};

        VkBuffer mInstanceInfoBuffers[kFramesInFlight]{};
        VmaAllocation mInstanceInfoAllocations[kFramesInFlight]{};
        std::byte *mInstanceInfoMapped[kFramesInFlight]{};

        std::vector<MeshBlas> mBlases;
    };
}
