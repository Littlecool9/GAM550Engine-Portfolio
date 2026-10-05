/**
 * @file MaterialStore.cpp
 * @author Sumin Park
 * @brief Material pipelines, set 0 and the shared pipeline layout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MaterialStore.h"
#include "ResourceStore.h"
#include "VulkanDevice.h"
#include "VulkanUtil.h"
#include "VulkanPushConstants.h"

#include <renderer/vulkan/VulkanConstants.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace mir::vk
{
    namespace
    {
        constexpr std::string_view kModelShaderName = "model";
    }

    bool MaterialStore::Initialize(VulkanDevice &device, ResourceStore &resources, VkFormat colorFormat,
        const std::array<VkDescriptorSetLayout, 3> &sharedLayouts)
    {
        mDevice = &device;

        if (!CreatePipelineLayout(sharedLayouts))
            return false;

        mDefaultMaterial = CreateMaterial(MaterialDesc{}, resources, colorFormat);
        if (mDefaultMaterial.IsNull())
            return false;

        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE,
            reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");
        return true;
    }

    void MaterialStore::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        // Before the pool: frees each material's set into it.
        DestroyMaterials();
        mDefaultMaterial = kNullMaterial;

        if (mDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mDescriptorPool, nullptr);
            mDescriptorPool = VK_NULL_HANDLE;
        }

        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }

        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }
    }

    bool MaterialStore::CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts)
    {
        // binding 0 = base color, binding 1 = normal map. Every material
        // uses this same layout, whether or not its shader samples them.
        const std::array<VkDescriptorSetLayoutBinding, 2> setBindings{ {
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } } };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(setBindings.size()),
            .pBindings = setBindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDescriptorSetLayout failed");
            return false;
        }

        // One set per material. A model primitive eats two (front and mirrored
        // front face), so this is not as generous as it looks.
        constexpr uint32_t kMaxMaterials = 1024;
        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kMaxMaterials * static_cast<uint32_t>(setBindings.size()) };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = kMaxMaterials,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };

        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDescriptorPool failed");
            return false;
        }

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(RasterPushConstants) };

        // set 0 = material (textures), set 1 = per-view camera UBO,
        // set 2 = per-draw model data, set 3 = shadow maps.
        const std::array<VkDescriptorSetLayout, 4> setLayouts{
            mSetLayout, sharedLayouts[0], sharedLayouts[1], sharedLayouts[2] };

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice->Handle(), &layoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreatePipelineLayout failed");
            return false;
        }

        return true;
    }

    VkPipeline MaterialStore::BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat)
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath(desc.shaderName + ".vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath(desc.shaderName + ".fragmentMain.spv"));

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
            return VK_NULL_HANDLE;
        }

        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vertModule,
             .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fragModule,
             .pName = "fragmentMain" } };

        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const std::array<VkVertexInputAttributeDescription, 6> vertexAttributes{ { { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) },
                                                                                 { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color) },
                                                                                 { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) },
                                                                                 { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv) },
                                                                                 { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(Vertex, alpha) },
                                                                                 { .location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, tangent) } } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            // Only the model shader reads uv, alpha and tangent. Declaring
            // them for the others is legal but makes the validation layer
            // complain about attributes no shader consumes.
            .vertexAttributeDescriptionCount =
                desc.shaderName == kModelShaderName ? static_cast<uint32_t>(vertexAttributes.size()) : 3u,
            .pVertexAttributeDescriptions = vertexAttributes.data() };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = desc.topology,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = desc.polygonMode,
            .cullMode = desc.cullMode,
            .frontFace = desc.frontFace,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        const VkPipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = desc.alphaMode == MaterialAlphaMode::Blend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = desc.alphaMode == MaterialAlphaMode::Blend ? VK_FALSE : VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_GREATER, // reverse-Z: nearer is larger
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE };

        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment };

        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };

        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorFormat,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depthStencil,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = mPipelineLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(
            mDevice->Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);

        if (result != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateGraphicsPipelines failed: {}", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        return pipeline;
    }

    MaterialHandle MaterialStore::CreateMaterial(const MaterialDesc &desc, ResourceStore &resources, VkFormat colorFormat)
    {
        const VkPipeline pipeline = BuildPipeline(desc, colorFormat);
        if (pipeline == VK_NULL_HANDLE)
            return kNullMaterial;

        const GpuTexture *baseColor = resources.FindTexture(desc.baseColorTexture);
        if (baseColor == nullptr)
            baseColor = resources.FindTexture(resources.DefaultWhiteTexture());

        const GpuTexture *normal = resources.FindTexture(desc.normalTexture);
        if (normal == nullptr)
            normal = resources.FindTexture(resources.DefaultNormalTexture());

        if (baseColor == nullptr || normal == nullptr)
        {
            MIR_LOG_CRITICAL("CreateMaterial: default textures unavailable");
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        // Shared with every other material that asked for the same state, so
        // this is not destroyed with the material.
        const VkSampler sampler = resources.AcquireSampler(desc.sampler);
        if (sampler == VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        const VkDescriptorSetAllocateInfo setAllocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mDescriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &mSetLayout };

        if (vkAllocateDescriptorSets(mDevice->Handle(), &setAllocInfo, &descriptorSet) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateMaterial: descriptor set allocation failed");
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        const VkDescriptorImageInfo baseColorInfo{
            .sampler = sampler,
            .imageView = baseColor->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo normalInfo{
            .sampler = sampler,
            .imageView = normal->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        const std::array<VkWriteDescriptorSet, 2> writes{ {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &baseColorInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &normalInfo } } };

        vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        const bool unlit = desc.shaderName == "unlit";
        // The model shader reads these out of set 2; see ModelDescriptors::Bind.
        const glm::vec4 parameters{ static_cast<float>(desc.alphaMode), desc.alphaCutoff,
            desc.cullMode == VK_CULL_MODE_NONE ? 1.0f : 0.0f, 0.0f };
        const bool modelShader = desc.shaderName == kModelShaderName;

        uint32_t index;
        uint32_t generation;
        if (!mFreeIndices.empty())
        {
            index = mFreeIndices.back();
            mFreeIndices.pop_back();
            generation = mMaterials[index].mGeneration;
            mMaterials[index] = GpuMaterial{ pipeline, descriptorSet, parameters, modelShader, generation, unlit };
        }
        else
        {
            index = static_cast<uint32_t>(mMaterials.size());
            generation = 1;
            mMaterials.push_back(GpuMaterial{ pipeline, descriptorSet, parameters, modelShader, generation, unlit });
        }

        MIR_LOG_INFO("Material created: {} (index {})", desc.shaderName, index);
        return MaterialHandle{ index, generation };
    }

    void MaterialStore::FreeMaterial(GpuMaterial &material)
    {
        // The sampler belongs to the resource store's cache, not to the material.
        if (material.mDescriptorSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(mDevice->Handle(), mDescriptorPool, 1, &material.mDescriptorSet);
        if (material.mPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), material.mPipeline, nullptr);
    }

    bool MaterialStore::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == mDefaultMaterial || FindMaterial(handle) == nullptr)
            return false;

        GpuMaterial &material = mMaterials[handle.mIndex];
        FreeMaterial(material);

        const uint32_t nextGeneration = material.mGeneration + 1;
        material = GpuMaterial{};
        material.mGeneration = nextGeneration; // every outstanding copy of handle is now stale

        mFreeIndices.push_back(handle.mIndex);
        return true;
    }

    std::size_t MaterialStore::ReleaseMaterials()
    {
        const GpuMaterial *const defaultMaterial = FindMaterial(mDefaultMaterial);
        if (defaultMaterial == nullptr)
        {
            const std::size_t count = mMaterials.size();
            DestroyMaterials();
            return count;
        }

        // Kept: draws and CreateMaterial fall back to it.
        const GpuMaterial kept = *defaultMaterial;
        std::size_t released = 0;
        for (std::size_t i = 0; i < mMaterials.size(); ++i)
        {
            if (i == mDefaultMaterial.mIndex)
                continue;
            FreeMaterial(mMaterials[i]);
            ++released;
        }

        mMaterials.clear();
        mMaterials.push_back(kept);
        mFreeIndices.clear();
        mDefaultMaterial = MaterialHandle{ 0, kept.mGeneration };
        return released;
    }

    void MaterialStore::DestroyMaterials()
    {
        for (GpuMaterial &material : mMaterials)
            FreeMaterial(material);
        mMaterials.clear();
        mFreeIndices.clear();
    }

    const GpuMaterial *MaterialStore::FindMaterial(MaterialHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mMaterials.size())
            return nullptr;

        const GpuMaterial &material = mMaterials[handle.mIndex];

        if (material.mGeneration != handle.mGeneration)
            return nullptr;

        return &material;
    }

    const GpuMaterial *MaterialStore::FindOrDefault(MaterialHandle handle) const
    {
        const GpuMaterial *material = FindMaterial(handle);
        if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
            material = FindMaterial(mDefaultMaterial);
        if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
            return nullptr;
        return material;
    }
}
