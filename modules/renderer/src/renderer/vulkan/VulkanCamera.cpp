/**
 * @file VulkanCamera.cpp
 * @author Sumin Park
 * @brief per-view camera UBO and light list.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <renderer/VulkanRenderer.h>
#include "VulkanUtil.h"

#include <core/log/Log.h>
#include <core/math/Align.h>

#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>

namespace mir
{
    namespace
    {
        struct CameraData
        {
            glm::mat4 viewProj;
            glm::vec4 lightDirWS;   // xyz = normalized direction *toward* the light/sun
            glm::vec4 lightColor;   // rgb = light color/intensity
            glm::vec4 ambientColor; // rgb = flat ambient term
            glm::mat4 invViewProj;
            glm::vec4 cameraPos;      // xyz = camera in render space: always the origin
            glm::vec4 fogColor;       // rgb = fog color, a = density (0 = disabled)
            glm::vec4 cameraWorldPos; // xyz = absolute camera, narrowed from the Scene origin; atmosphere only
            glm::vec4 clipPlanes;     // x = near, y = far, or 0 for no far plane; ray tracing only
            glm::vec4 atmosphereScattering; // xyz = Rayleigh coeff (m^-1), w = Mie coeff (m^-1)
            glm::vec4 atmosphereRadii;      // x = planet radius, y = atmosphere radius, z = Rayleigh scale height, w = Mie scale height (m)
            glm::vec4 atmosphereMisc;       // x = Mie anisotropy g, y = sun intensity
            glm::vec4 lightingMeta;         // x = this view's Point/Spot/Area count, y = its first index in gLights,
                                            // z = 1 if the Sun casts a shadow, w = this view's first spot shadow matrix
            glm::mat4 sunShadowViewProj;
        };

        // Must match GpuLight in assets/shaders/LightingCommon.slang.
        struct GpuLight
        {
            glm::vec4 positionAndRange;    // xyz = relative to the view's camera, w = range
            glm::vec4 directionAndSpotCos; // xyz = normalized aim direction, w = cos(half-angle); -1 = no cone
            glm::vec4 color;               // rgb = color * intensity, w = shadowSlot (see LightItem::shadowSlot)
        };
    }

    bool VulkanRenderer::CreateCameraResources()
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        mCameraAlignedSize = AlignUp(sizeof(CameraData), props.limits.minUniformBufferOffsetAlignment);

        VkShaderStageFlags cameraStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkShaderStageFlags lightListStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (mRayTracingSupported)
        {
            cameraStages |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                VK_SHADER_STAGE_MISS_BIT_KHR;
            lightListStages |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        }

        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{ {
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
             .descriptorCount = 1,
             .stageFlags = cameraStages },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags = lightListStages } } };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice, &setLayoutInfo, nullptr, &mCameraSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateCameraResources: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const std::array<VkDescriptorPoolSize, 2> poolSizes{ {
            { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = kFramesInFlight },
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = kFramesInFlight } } };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data() };

        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mCameraDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateCameraResources: vkCreateDescriptorPool failed");
            return false;
        }

        const VkDeviceSize bufferSize = mCameraAlignedSize * kMaxViewsPerFrame;
        const VkDeviceSize lightBufferSize = sizeof(GpuLight) * kMaxLights * kMaxViewsPerFrame;

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VmaAllocationInfo allocInfo{};
            if (!vk::CreateBuffer(mAllocator, bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mCameraBuffers[i], mCameraAllocations[i], &allocInfo))
            {
                MIR_LOG_CRITICAL("CreateCameraResources: camera buffer allocation failed for frame {}", i);
                return false;
            }
            mCameraMapped[i] = static_cast<std::byte *>(allocInfo.pMappedData);

            VmaAllocationInfo lightAllocInfo{};
            if (!vk::CreateBuffer(mAllocator, lightBufferSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mLightBuffers[i], mLightAllocations[i], &lightAllocInfo))
            {
                MIR_LOG_CRITICAL("CreateCameraResources: light buffer allocation failed for frame {}", i);
                return false;
            }
            mLightMapped[i] = static_cast<std::byte *>(lightAllocInfo.pMappedData);

            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mCameraDescriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mCameraSetLayout };

            if (vkAllocateDescriptorSets(mDevice, &setAllocInfo, &mCameraDescriptorSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateCameraResources: descriptor set allocation failed for frame {}", i);
                return false;
            }

            // range = one view's slice; the dynamic offset walks between them.
            const VkDescriptorBufferInfo bufferInfo{
                .buffer = mCameraBuffers[i],
                .offset = 0,
                .range = sizeof(CameraData) };

            const VkDescriptorBufferInfo lightBufferInfo{
                .buffer = mLightBuffers[i],
                .offset = 0,
                .range = lightBufferSize };

            const std::array<VkWriteDescriptorSet, 2> writes{ {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mCameraDescriptorSets[i],
                 .dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                 .pBufferInfo = &bufferInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mCameraDescriptorSets[i],
                 .dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &lightBufferInfo } } };

            vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mCameraBuffers[i]),
                std::format("Camera UBO {}", i).c_str());
            NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mLightBuffers[i]),
                std::format("Light list {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyCameraResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mCameraBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mCameraBuffers[i], mCameraAllocations[i]);
                mCameraBuffers[i] = VK_NULL_HANDLE;
                mCameraAllocations[i] = VK_NULL_HANDLE;
                mCameraMapped[i] = nullptr;
            }

            if (mLightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mLightBuffers[i], mLightAllocations[i]);
                mLightBuffers[i] = VK_NULL_HANDLE;
                mLightAllocations[i] = VK_NULL_HANDLE;
                mLightMapped[i] = nullptr;
            }
        }

        // Freeing the pool frees its descriptor sets too.
        if (mCameraDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice, mCameraDescriptorPool, nullptr);
            mCameraDescriptorPool = VK_NULL_HANDLE;
        }

        if (mCameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice, mCameraSetLayout, nullptr);
            mCameraSetLayout = VK_NULL_HANDLE;
        }
    }

    void VulkanRenderer::WriteCameraData(const RenderView &view, uint32_t viewIndex)
    {
        // The atmosphere needs the camera's altitude over its planet, where
        // float's 1/64 m at the map edge is invisible; no geometry reads it.
        const glm::vec3 cameraWorld =
            TryCameraRelativeFloat(view.cameraPosition, WorldPosition{}).value_or(glm::vec3(0.0f));

        const uint32_t firstLight = viewIndex * kMaxLights;
        const uint32_t lightCount = WriteViewLights(view, viewIndex);

        glm::mat4 sunShadowViewProjForView{ 1.0f };
        if (mHasSunShadow)
        {
            const glm::vec3 anchorFromView =
                TryCameraRelativeFloat(mSunShadowAnchor, view.cameraPosition).value_or(glm::vec3(0.0f));
            sunShadowViewProjForView = mSunShadowViewProj * glm::translate(glm::mat4(1.0f), -anchorFromView);
        }

        const uint32_t spotShadowBase = viewIndex * kMaxSpotShadows;
        glm::mat4 *const spotMatrixDst =
            reinterpret_cast<glm::mat4 *>(mSpotShadowMatrixMapped[mFrameIndex]) + spotShadowBase;
        for (uint32_t slot = 0; slot < kMaxSpotShadows; ++slot)
        {
            const SpotShadowCaster &caster = mSpotShadowCasters[slot];
            if (!caster.active)
                continue;
            const glm::vec3 anchorFromView =
                TryCameraRelativeFloat(caster.anchor, view.cameraPosition).value_or(glm::vec3(0.0f));
            spotMatrixDst[slot] = caster.viewProj * glm::translate(glm::mat4(1.0f), -anchorFromView);
        }
        vmaFlushAllocation(mAllocator, mSpotShadowMatrixAllocations[mFrameIndex],
            sizeof(glm::mat4) * spotShadowBase, sizeof(glm::mat4) * kMaxSpotShadows);

        const CameraData data{
            .viewProj = view.viewProj,
            .lightDirWS = glm::vec4(mLightDirection, 0.0f),
            .lightColor = glm::vec4(mLightColor, 0.0f),
            .ambientColor = glm::vec4(mAmbientColor, 0.0f),
            .invViewProj = glm::inverse(view.viewProj),
            .cameraPos = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
            .fogColor = glm::vec4(mFogColor, mFogDensity),
            .cameraWorldPos = glm::vec4(cameraWorld, 1.0f),
            .clipPlanes = glm::vec4(view.nearZ, view.farZ, 0.0f, 0.0f),
            .atmosphereScattering = glm::vec4(mAtmosphere.mRayleighCoeff * 1e-6f, mAtmosphere.mMieCoeff * 1e-6f),
            .atmosphereRadii = glm::vec4(mAtmosphere.mPlanetRadiusKm * 1e3f, mAtmosphere.mAtmosphereRadiusKm * 1e3f,
                mAtmosphere.mRayleighScaleHeightKm * 1e3f, mAtmosphere.mMieScaleHeightKm * 1e3f),
            .atmosphereMisc = glm::vec4(mAtmosphere.mMieG, mAtmosphere.mSunIntensity, 0.0f, 0.0f),
            .lightingMeta = glm::vec4(static_cast<float>(lightCount), static_cast<float>(firstLight),
                mHasSunShadow ? 1.0f : 0.0f, static_cast<float>(spotShadowBase)),
            .sunShadowViewProj = sunShadowViewProjForView };
        const VkDeviceSize offset = static_cast<VkDeviceSize>(viewIndex) * mCameraAlignedSize;

        std::byte *const dst = mCameraMapped[mFrameIndex] + offset;
        std::memcpy(dst, &data, sizeof(CameraData));

        // No-op if the memory type is already host-coherent; required
        // otherwise, since nothing else guarantees this write is flushed
        // before the render commands using it are submitted.
        vmaFlushAllocation(mAllocator, mCameraAllocations[mFrameIndex], offset, sizeof(CameraData));
    }

    void VulkanRenderer::SetLights(std::span<const LightItem> lights)
    {
        const uint32_t count = std::min(static_cast<uint32_t>(lights.size()), kMaxLights);

        if (count != mLights.size())
            MIR_LOG_INFO("Point/Spot/Area lights active: {}", count);
        // Only staged: the buffer may still be in use by this frame slot's last
        // submission until DrawFrame/TraceFrame have waited for it.
        mLights.assign(lights.begin(), lights.begin() + count);
    }

    uint32_t VulkanRenderer::WriteViewLights(const RenderView &view, uint32_t viewIndex)
    {
        GpuLight *const dst = reinterpret_cast<GpuLight *>(mLightMapped[mFrameIndex]) + viewIndex * kMaxLights;

        // A light too far from this camera for float is skipped, like a draw item.
        uint32_t count = 0;
        for (const LightItem &light : mLights)
        {
            const std::optional<glm::vec3> position = TryCameraRelativeFloat(light.position, view.cameraPosition);
            if (!position)
                continue;
            dst[count++] = GpuLight{
                .positionAndRange = glm::vec4(*position, light.range),
                .directionAndSpotCos = glm::vec4(light.direction, light.spotCos),
                .color = glm::vec4(light.color, static_cast<float>(light.shadowSlot)) };
        }

        if (count > 0)
        {
            const VkDeviceSize offset = sizeof(GpuLight) * viewIndex * kMaxLights;
            vmaFlushAllocation(mAllocator, mLightAllocations[mFrameIndex], offset, sizeof(GpuLight) * count);
        }
        return count;
    }
}
