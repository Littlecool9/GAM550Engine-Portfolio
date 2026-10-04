/**
 * @file ImageImporter.h
 * @author Quy Dong Ta
 * @brief PNG and JPEG decoding to the RGBA8 texture payload, shared by the
 *        glTF importer and the cooker's standalone images.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <assets/ModelAsset.h>
#include <cstddef>
#include <filesystem>
#include <span>

namespace mir::cook
{
    // Throws on an image that is not a png or jpeg, is corrupt, or is over
    // 64 megapixels. Any channel count decodes to RGBA8.
    TextureAsset DecodeImage(std::span<const std::byte> bytes);

    //.png, .jpg and .jpeg, in any case: what DecodeImage reads
    bool IsImagePath(const std::filesystem::path &path);
} // namespace mir::cook
