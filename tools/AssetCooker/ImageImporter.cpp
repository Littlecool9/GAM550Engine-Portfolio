/**
 * @file ImageImporter.cpp
 * @author Quy Dong Ta
 * @brief PNG and JPEG decoding to the RGBA8 texture payload, shared by the
 *        glTF importer and the cooker's standalone images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ImageImporter.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <algorithm>
#include <cctype>
#include <climits>
#include <memory>
#include <stb_image.h>
#include <stdexcept>
#include <string>

namespace mir::cook
{
    TextureAsset DecodeImage(std::span<const std::byte> bytes)
    {
        if (bytes.size() > INT_MAX)
            throw std::runtime_error("encoded image too large");
        const auto *data = reinterpret_cast<const stbi_uc *>(bytes.data());
        const int size = static_cast<int>(bytes.size());
        int w = 0, h = 0, n = 0;
        if (stbi_info_from_memory(data, size, &w, &h, &n) == 0 || w <= 0 || h <= 0 ||
            uint64_t(w) * h > 64 * 1024 * 1024)
            throw std::runtime_error("invalid/oversized PNG or JPEG");
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(data, size, &w, &h, &n, 4),
                                                                    stbi_image_free);
        if (pixels == nullptr)
            throw std::runtime_error("image decode failed");
        return TextureAsset{
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), {pixels.get(), pixels.get() + size_t(w) * h * 4}};
    }

    bool IsImagePath(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
    }
} // namespace mir::cook
