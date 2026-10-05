/**
 * @file ShadowMaps.h
 * @author Sumin Park
 * @brief Set 3: Sun, spot and point shadow maps, their casters and depth passes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "CameraBuffer.h"

#include <renderer/VulkanRenderer.h>
#include <renderer/vulkan/VulkanConstants.h>

#include <glm/mat4x4.hpp>
#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;

    class ShadowMaps
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        // The spans are borrowed until the next frame records its passes.
        void SetSunCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor, std::span<const DrawItem> items);
        void SetSpotCaster(uint32_t slot, bool active, const glm::mat4 &viewProj, WorldPosition anchor,
            std::span<const DrawItem> items);
        void SetPointCaster(uint32_t slot, bool active, WorldPosition position, float range,
            const std::array<PointShadowFace, 6> &faces);

        void RecordPasses(VkCommandBuffer cmd, uint32_t frame, const ResourceStore &resources);

        // Writes this view's spot matrices, relative to its camera, into the frame's buffer.
        ViewShadowInfo WriteView(uint32_t frame, const RenderView &view, uint32_t viewIndex);

        VkDescriptorSetLayout SetLayout() const { return mSetLayout; }
        VkDescriptorSet Set(uint32_t frame) const { return mSets[frame]; }

    private:
        constexpr static uint32_t kSunShadowResolution = 2048;
        constexpr static uint32_t kSpotShadowResolution = 1024;
        constexpr static uint32_t kPointShadowResolution = 512;

        Device *mDevice = nullptr;

        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mDepthPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mDepthPipeline = VK_NULL_HANDLE;
        VkPipeline mPointPipeline = VK_NULL_HANDLE; // shadowDistance.slang

        VkImage mSunImages[kFramesInFlight]{};
        VmaAllocation mSunAllocations[kFramesInFlight]{};
        VkImageView mSunViews[kFramesInFlight]{};

        bool mHasSun = false;
        glm::mat4 mSunViewProj{ 1.0f };
        WorldPosition mSunAnchor;
        std::span<const DrawItem> mSunItems;

        VkImage mSpotImages[kFramesInFlight]{};
        VmaAllocation mSpotAllocations[kFramesInFlight]{};
        VkImageView mSpotArrayViews[kFramesInFlight]{}; // sampling: 2D_ARRAY over all slots
        VkImageView mSpotLayerViews[kFramesInFlight][kMaxSpotShadows]{}; // rendering: one slot each

        VkBuffer mSpotMatrixBuffers[kFramesInFlight]{};
        VmaAllocation mSpotMatrixAllocations[kFramesInFlight]{};
        std::byte *mSpotMatrixMapped[kFramesInFlight]{};

        struct SpotCaster
        {
            bool active = false;
            glm::mat4 viewProj{ 1.0f }; // relative to `anchor`, not any view's camera
            WorldPosition anchor;       // the light's own position
            std::span<const DrawItem> items;
        };
        std::array<SpotCaster, kMaxSpotShadows> mSpotCasters{};

        VkImage mPointDepthImages[kFramesInFlight]{};
        VmaAllocation mPointDepthAllocations[kFramesInFlight]{};
        VkImageView mPointDepthLayerViews[kFramesInFlight][kMaxPointShadows * 6]{};

        VkImage mPointColorImages[kFramesInFlight]{};
        VmaAllocation mPointColorAllocations[kFramesInFlight]{};
        VkImageView mPointColorLayerViews[kFramesInFlight][kMaxPointShadows * 6]{};
        VkImageView mPointCubeArrayViews[kFramesInFlight]{}; // sampling: CUBE_ARRAY

        struct PointCaster
        {
            bool active = false;
            WorldPosition position; // the light itself: every face's "camera" sits here
            float range = 0.0f;     // for the shading pass to normalize sampled distance by
            std::array<PointShadowFace, 6> faces{};
        };
        std::array<PointCaster, kMaxPointShadows> mPointCasters{};
    };
}
