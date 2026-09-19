/**
 * @file Texture.h
 * @author Rahul Nair
 * @brief Texture handle and the CPU-side description that uploads one.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mir
{
    struct TextureHandle
    {
        static constexpr uint32_t kNullIndex = UINT32_MAX;

        uint32_t mIndex = kNullIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kNullIndex; }
        constexpr bool operator==(const TextureHandle &) const = default;
    };

    inline constexpr TextureHandle kNullTexture{};

    // CPU-side pixels for CreateTexture.
    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;

        // width * height * 4 bytes, row-major starting at the top-left texel.
        std::span<const std::byte> pixels;

        // Base color textures are authored in sRGB.
        bool srgb = true;
    };
}
