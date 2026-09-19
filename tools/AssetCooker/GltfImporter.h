#pragma once
#include <assets/AssetManifest.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mir::cook
{
    struct ImportedBlob
    {
        AssetId id;
        std::vector<std::byte> bytes;
    };
    struct GltfImport
    {
        std::vector<ImportedBlob> blobs;
        std::filesystem::path metadataPath;
        std::string metadata;
    };
    // No output is published until all conversion/validation has succeeded.
    GltfImport ImportGltf(const std::filesystem::path &path, const std::string &assetPath,
                          const std::vector<std::filesystem::path> &cookedRoots);
} // namespace mir::cook
