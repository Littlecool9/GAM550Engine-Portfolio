/**
 * @file TerrainAssetPath.h
 * @author Jackey Cen
 * @brief Maps a terrain recipe path to its asset id, and a game to its releases root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <assets/AssetPath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mir
{
    inline constexpr std::string_view kTerrainRecipeSuffix = ".terrain.json";

    enum class TerrainAssetPathError : uint8_t
    {
        None,
        IdRoot,     // pathError says why
        SourcePath, // pathError says why; includes ".." out of the id root
        NotARecipe, // does not end with ".terrain.json" spelled exactly so
    };

    struct TerrainAssetPath
    {
        // The string the id hashes, case kept.
        std::string logicalPath;
        AssetId asset;
        TerrainAssetPathError error = TerrainAssetPathError::None;
        AssetPathError pathError = AssetPathError::None;

        bool Ok() const { return error == TerrainAssetPathError::None; }
    };

    // The cooker's rule for any asset; case is kept and hashed, so "Hills" and "hills" are two assets.
    TerrainAssetPath ResolveTerrainAssetPath(std::string_view idRoot, std::string_view sourcePath);

    // Empty when path is ok.
    std::string DescribeTerrainAssetPathError(const TerrainAssetPath &path, std::string_view idRoot,
                                              std::string_view sourcePath);

    // Beside the game's assetsRoot, never inside it, so nothing cooks or watches it.
    std::filesystem::path TerrainReleasesRoot(const std::filesystem::path &gameDir);

    // A recipe or release file; case-insensitive, either separator.
    bool IsTerrainFileExcludedFromCook(std::string_view path);
}
