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
        AssetId modelId{};
        std::string identityPath;
        std::vector<std::string> aliases;
        std::vector<std::filesystem::path> dependencies;
        std::vector<ImportedBlob> blobs;
        std::filesystem::path metadataPath;
        std::string metadata;
    };
    // No output is published until all conversion/validation has succeeded.
    GltfImport ImportGltf(const std::filesystem::path &path, const std::string &assetPath,
                          const std::vector<std::filesystem::path> &cookedRoots);

    // Resolves URI dependencies without loading them. Every returned path is
    // canonical and confined to one of allowedRoots.
    std::vector<std::filesystem::path> InspectGltfDependencies(const std::filesystem::path &path,
                                                               const std::vector<std::filesystem::path> &allowedRoots);
} // namespace mir::cook
