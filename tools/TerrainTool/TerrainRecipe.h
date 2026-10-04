/**
 * @file TerrainRecipe.h
 * @author Jackey Cen
 * @brief Terrain source recipe: JSON validation, canonical text and source fingerprint.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    inline constexpr uint32_t kTerrainRecipeVersion = 1;
    // Bumping it changes every fingerprint.
    inline constexpr uint32_t kRecipeCanonicalVersion = 1;
    inline constexpr std::string_view kReliefGeneratorId = "mir.relief";
    // Bump when anything changes the samples.
    inline constexpr uint32_t kReliefGeneratorVersion = 1;
    // float32 still resolves ~1 mm here.
    inline constexpr double kMaxHeightMagnitude = 10000.0;

    struct NoiseTerm
    {
        double amplitude = 0.0;  // m, [0, 10000]
        double wavelength = 1.0; // m, [0.01, 1e6]; finest octave >= 2 * sampleSpacing
        int32_t octaves = 1;     // [1, 8]
    };

    // "mir.relief" v1; formula in TerrainGenerator.cpp. Must stay within kMaxHeightMagnitude.
    struct ReliefParameters
    {
        double baseHeight = 0.0;   // m, [-10000, 10000]
        double tiltX = 0.0;        // m per m, [-1, 1]
        double tiltZ = 0.0;        // m per m, [-1, 1]
        NoiseTerm broad;           // world-scale relief, fBm in [-1, 1]
        NoiseTerm ridges;          // sharp crest lines, ridged in [0, 1]
        NoiseTerm detail;          // local variation, fBm in [-1, 1]
        double plainsHeight = 0.0; // m, [-10000, 10000]
        double plainsBlend = 0.0;  // m, [0, 1000]; 0 is a hard floor
    };

    // Every field is required, so no hidden default can change a fingerprint.
    struct TerrainRecipe
    {
        uint32_t recipeVersion = kTerrainRecipeVersion;
        std::string generatorId;
        uint32_t generatorVersion = 0;
        uint64_t seed = 0;
        TerrainLayout layout;
        ReliefParameters relief;
    };

        // Dotted JSON path; empty for the whole document.
    struct RecipeIssue
    {
        std::string field;
        std::string message;
    };

    struct RecipeParseResult
    {
        std::optional<TerrainRecipe> recipe;
        std::vector<RecipeIssue> issues;
    };

    // Reports every bad field in one pass; never clamps.
    RecipeParseResult ParseTerrainRecipe(std::string_view json);
    RecipeParseResult LoadTerrainRecipe(const std::filesystem::path &path);

    // Sorted "key=value" lines; doubles as bit patterns, so JSON spelling does not matter.
    std::string CanonicalRecipeText(const TerrainRecipe &recipe);
    TerrainSourceFingerprint FingerprintRecipe(const TerrainRecipe &recipe);
}
