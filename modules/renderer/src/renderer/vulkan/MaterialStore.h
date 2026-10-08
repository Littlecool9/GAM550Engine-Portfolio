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
#include <string_view>
#include <vector>

namespace mir::vk
{
    class Device;
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
        // sharedLayouts are sets 1-3, borrowed for the pipeline layouts; their owners destroy them.
        // Also creates the default and error materials and the sky pipeline, which reads set 1 alone.
        bool Initialize(Device &device, ResourceStore &resources, VkFormat colorFormat,
            const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
        void Shutdown();

        MaterialHandle CreateMaterial(const MaterialDesc &desc, ResourceStore &resources, VkFormat colorFormat);
        // False when the handle is null, stale, the default or the error material.
        bool DestroyMaterial(MaterialHandle handle);
        // Every material but the default and error ones, which move to slots 0 and 1; returns how many were released.
        std::size_t ReleaseMaterials();

        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        // What a draw with `handle` uses: the material; else the default for a null handle and the error
        // material for a stale one; null only when both are gone.
        const GpuMaterial *FindOrDefault(MaterialHandle handle) const;

        MaterialHandle DefaultMaterial() const { return mDefaultMaterial; }
        MaterialHandle ErrorMaterial() const { return mErrorMaterial; }
        VkPipelineLayout PipelineLayout() const { return mPipelineLayout; }

        VkPipeline SkyPipeline() const { return mSkyPipeline; }
        VkPipelineLayout SkyPipelineLayout() const { return mSkyPipelineLayout; }

    private:
        bool CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts);
        // The state a material pipeline and the sky pipeline differ in.
        struct PipelineState
        {
            std::string_view shaderName;
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            VkPipelineColorBlendAttachmentState blend{};
            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            VkPipelineLayout layout = VK_NULL_HANDLE;
        };

        VkPipeline CreateGraphicsPipeline(const PipelineState &state, VkFormat colorFormat);
        VkPipeline BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat);
        bool CreateSkyPipeline(VkDescriptorSetLayout cameraSetLayout, VkFormat colorFormat);
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMaterial(GpuMaterial &material);
        void DestroyMaterials();

        Device *mDevice = nullptr;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mSkyPipeline = VK_NULL_HANDLE;

        // set 0: binding 0 = base color, binding 1 = normal map.
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;

        std::vector<GpuMaterial> mMaterials;
        std::vector<uint32_t> mFreeIndices;

        MaterialHandle mDefaultMaterial;
        MaterialHandle mErrorMaterial;
    };
}
