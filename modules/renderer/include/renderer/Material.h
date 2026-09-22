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

    struct MaterialDesc
    {
        std::string shaderName = "default";

        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

        TextureHandle baseColorTexture;
        TextureHandle normalTexture;
    };
}
