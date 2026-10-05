/**
 * @file MaterialStore.h
 * @author Sumin Park
 * @brief Materials, their pipelines, set 0 and the pipeline layout every material shares.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>

#include <glm/vec4.hpp>
#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mir::vk
{
    class VulkanDevice;
    class ResourceStore;

    struct GpuMaterial
    {
        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
        // alphaMode, alphaCutoff, doubleSided, unused. Handed to the model
        // shader through set 2, since the push constant range is full.
        glm::vec4 mParameters{};
        bool mModelShader = false;
        uint32_t mGeneration = 0;
        bool mUnlit = false;
    };

    // Destroy* and Release* do not wait: the caller idles the device first,
    // since in-flight frames may still use the pipelines and sets they free.
    class MaterialStore
    {
    public:
        // sharedLayouts are sets 1-3, borrowed for the pipeline layout; their owners destroy them.
        // Also creates the default material.
        bool Initialize(VulkanDevice &device, ResourceStore &resources, VkFormat colorFormat,
            const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
        void Shutdown();

        MaterialHandle CreateMaterial(const MaterialDesc &desc, ResourceStore &resources, VkFormat colorFormat);
        // False when the handle is null, stale or the default material.
        bool DestroyMaterial(MaterialHandle handle);
        // Every material but the default, which moves to slot 0; returns how many were released.
        std::size_t ReleaseMaterials();

        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        // What a draw with `handle` uses: the material, else the default; null only when the default is gone too.
        const GpuMaterial *FindOrDefault(MaterialHandle handle) const;

        MaterialHandle DefaultMaterial() const { return mDefaultMaterial; }
        VkPipelineLayout PipelineLayout() const { return mPipelineLayout; }

    private:
        bool CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
        VkPipeline BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat);
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMaterial(GpuMaterial &material);
        void DestroyMaterials();

        VulkanDevice *mDevice = nullptr;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        // set 0: binding 0 = base color, binding 1 = normal map.
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;

        std::vector<GpuMaterial> mMaterials;
        std::vector<uint32_t> mFreeIndices;

        MaterialHandle mDefaultMaterial;
    };
}
