/**
 * @file TerrainValidate.h
 * @author Jackey Cen
 * @brief Validates a baked terrain candidate: integrity, coverage, finite heights, seams and identity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainContinuity.h"
#include "TerrainRecipe.h"
#include "TerrainReport.h"

#include <filesystem>

namespace mir::terrainbake
{
    struct TerrainValidateOptions
    {
        // When set, its fingerprint and layout must match.
        const TerrainRecipe *recipe = nullptr;
        // Published release directories have no candidate.json.
        bool requireManifest = true;
    };

    // Reads only through TerrainReleaseHandle; writes nothing.
    void ValidateTerrainCandidate(const std::filesystem::path &dir, const TerrainValidateOptions &options,
                                  TerrainReport &report);

    struct TerrainValidateRequest
    {
        std::filesystem::path candidateDir;
        // Empty: no recipe check.
        std::filesystem::path recipePath;
    };

    TerrainReport RunTerrainValidate(const TerrainValidateRequest &request);
}
