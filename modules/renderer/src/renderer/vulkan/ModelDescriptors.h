/**
 * @file ModelDescriptors.h
 * @author Jackey Cen
 * @brief Set 2: per-draw model data, with a pool and buffer list per frame in flight.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/vulkan/VulkanConstants.h>

#include <glm/vec4.hpp>
#include <volk.h>

#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class VulkanDevice;

    class ModelDescriptors
    {
    public:
        bool Initialize(VulkanDevice &device);
        void Shutdown();

        // Frees the frame's buffers and resets its pool; the frame slot must have been waited on.
        void ResetFrame(uint32_t frame);
        void ResetFrames();

        // Uploads one draw's deformation, with the material parameters in slot 2, and binds it as set 2.
        bool Bind(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame,
            std::span<const glm::vec4> deformation, const glm::vec4 &materialParameters);

        VkDescriptorSetLayout SetLayout() const { return mSetLayout; }

    private:
        VulkanDevice *mDevice = nullptr;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPools[kFramesInFlight]{};

        struct FrameBuffer { VkBuffer buffer; VmaAllocation allocation; };
        std::vector<FrameBuffer> mBuffers[kFramesInFlight];
    };
}
