/**
 * @file Material.h
 * @author Sumin Park
 * @brief Material handle and the description that builds one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

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
        // Compiled entry points are expected to be named vertexMain/fragmentMain
        std::string shaderName = "triangle";

        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        uint32_t texture = UINT32_MAX;
        uint32_t wrapS = 10497, wrapT = 10497, minFilter = 9729, magFilter = 9729;
        uint32_t alphaMode = 0;
        float alphaCutoff = 0.5f;
    };
}
