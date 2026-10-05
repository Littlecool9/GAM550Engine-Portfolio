/**
 * @file CameraBuffer.h
 * @author Sumin Park
 * @brief Set 1: the per-view camera UBO and light list.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/VulkanRenderer.h>
#include "VulkanConstants.h"

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    // What the camera data needs from the shadow maps for one view.
    struct ViewShadowInfo
    {
        bool hasSunShadow = false;
        glm::mat4 sunShadowViewProj{ 1.0f }; // relative to this view's camera
        uint32_t spotShadowBase = 0;         // this view's first matrix in set 3's spot buffer
    };

    class CameraBuffer
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        // All only staged: a frame slot's buffer may still be in use until that slot's wait.
        void SetLights(std::span<const LightItem> lights);
        void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color);
        void SetAmbientColor(const glm::vec3 &color) { mAmbientColor = color; }
        void SetFog(const glm::vec3 &color, float density);
        void SetAtmosphere(const Sky &sky) { mAtmosphere = sky; }

        // Fills this view's slice of the frame's buffers; returns its dynamic offset into binding 0.
        uint32_t WriteView(uint32_t frame, const RenderView &view, uint32_t viewIndex, const ViewShadowInfo &shadow);

        VkDescriptorSetLayout SetLayout() const { return mSetLayout; }
        VkDescriptorSet Set(uint32_t frame) const { return mSets[frame]; }

    private:
        uint32_t WriteViewLights(uint32_t frame, const RenderView &view, uint32_t viewIndex);

        Device *mDevice = nullptr;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        // binding 0: one CameraData per view, as a dynamic uniform buffer.
        VkBuffer mCameraBuffers[kFramesInFlight]{};
        VmaAllocation mCameraAllocations[kFramesInFlight]{};
        std::byte *mCameraMapped[kFramesInFlight]{};
        VkDeviceSize mCameraAlignedSize = 0;

        // binding 1: per view; each view writes its own kMaxLights slice, narrowed against its camera.
        constexpr static uint32_t kMaxLights = 64;
        VkBuffer mLightBuffers[kFramesInFlight]{};
        VmaAllocation mLightAllocations[kFramesInFlight]{};
        std::byte *mLightMapped[kFramesInFlight]{};
        std::vector<LightItem> mLights;

        // Scene-wide state every view repeats in its camera data.
        glm::vec3 mLightDirection{ glm::normalize(glm::vec3(0.4f, 0.7f, 0.5f)) };
        glm::vec3 mLightColor{ 0.85f, 0.85f, 0.85f };
        glm::vec3 mAmbientColor{ 0.25f, 0.25f, 0.25f };
        glm::vec3 mFogColor{ 0.02f, 0.02f, 0.02f };
        float mFogDensity = 0.0f;
        Sky mAtmosphere{};
    };
}
