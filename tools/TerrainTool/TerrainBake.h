/**
 * @file TerrainBake.h
 * @author Jackey Cen
 * @brief Bakes a terrain recipe into final tile heights, an encoded release and a candidate directory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainCandidate.h"
#include "TerrainRecipe.h"
#include "TerrainReport.h"

#include <assets/AssetId.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TileHeights.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    struct TerrainBakeOptions
    {
        // Fixed for reproducible release ids; unset mints one.
        std::optional<uint64_t> nonce;
        TerrainPackWriteOptions pack;
    };

    struct TerrainBakedRelease
    {
        TerrainReleaseInfo info;
        TerrainLayout layout;
        uint64_t nonce = 0;
        // Every layout tile in (z, x) order.
        std::vector<TileHeights> tiles;
        TerrainEncodedRelease encoded;
    };

    // P1: final heights are the source heights unchanged. Fills the "generate" check.
    std::optional<std::vector<TileHeights>> BakeFinalHeights(const TerrainRecipe &recipe, TerrainReport &report);

    // Fills the "encode" check.
    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainLayout &layout, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report);

    TerrainCandidateManifest MakeCandidateManifest(const TerrainBakedRelease &baked, std::string assetPath,
                                                   std::string recipePath);

    struct TerrainBakeRequest
    {
        std::filesystem::path recipePath;
        // Same id rule as cooked assets.
        std::string idRoot;
        std::string sourcePath;
        std::filesystem::path candidateDir;
        TerrainBakeOptions options;
    };

    // Builds a .partial sibling, then renames it, so candidateDir is absent or complete.
    bool BakeTerrainCandidate(const TerrainBakeRequest &request, TerrainReport &report);

    TerrainReport RunTerrainBake(const TerrainBakeRequest &request);

    // Set when the file on disk differs from sourcePath only in case; ids keep case.
    std::optional<std::string> RecipeSpellingMismatch(const std::filesystem::path &recipePath,
                                                      std::string_view sourcePath);

    // Problems go to the "arguments" check; nullopt when any argument is wrong.
    std::optional<TerrainAssetPath> CheckRecipeArguments(const std::filesystem::path &recipePath,
                                                         std::string_view idRoot, std::string_view sourcePath,
                                                         TerrainReport &report);
}
