/**
 * @file Material.h
 * @author Sumin Park
 * @brief Material handle and the description that builds one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Texture.h>

#include <volk.h>

#include <cstdint>
#include <string>

namespace mir
{
    // kNullMaterial = default material
    struct MaterialHandle
    {
        static constexpr uint32_t kNullIndex = UINT32_MAX;

        uint32_t mIndex = kNullIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kNullIndex; }
        constexpr bool operator==(const MaterialHandle &) const = default;
    };

    inline constexpr MaterialHandle kNullMaterial{};

    struct SamplerDesc
    {
        VkFilter magFilter = VK_FILTER_LINEAR;
        VkFilter minFilter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        bool useMips = false;

        constexpr bool operator==(const SamplerDesc &) const = default;
    };

    enum class MaterialAlphaMode : uint32_t
    {
        Opaque,
        Mask,
        Blend
    };

    struct MaterialDesc
    {
        std::string shaderName = "default";

        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // A null handle falls back to the renderer's default white / flat normal.
        TextureHandle baseColorTexture;
        TextureHandle normalTexture;

        SamplerDesc sampler;

        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        float alphaCutoff = 0.5f;
    };
}
