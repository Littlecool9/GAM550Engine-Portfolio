/**
 * @file TerrainSync.h
 * @author Jackey Cen
 * @brief Publishes a recipe only when the current release no longer matches it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainBake.h"
#include "TerrainPublish.h"
#include "TerrainReport.h"

#include <assets/AssetId.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRelease.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace mir::terrainbake
{
    struct TerrainSyncRequest
    {
        std::filesystem::path recipePath;
        std::string idRoot;
        std::string sourcePath;
        std::filesystem::path gameDir;
        // Removed and rebaked whenever sync publishes.
        std::filesystem::path candidateDir;
        // Debug and Release may sync the same asset at once; the second waits.
        std::chrono::milliseconds lockWait{std::chrono::minutes(10)};
        TerrainBakeOptions options;
        std::optional<TerrainPublishFault> fault;
    };

    // Empty when the current release matches. Reads the index and stats the packs only.
    std::optional<std::string> TerrainCurrentMismatch(const std::filesystem::path &releasesRoot, AssetId asset,
                                                      TerrainSourceFingerprint fingerprint,
                                                      std::optional<TerrainReleaseId> &current,
                                                      std::optional<TerrainReleaseHandle> *opened = nullptr);

    // A no-op when current matches; otherwise bakes, validates and publishes. Only adds releases.
    TerrainReport RunTerrainSync(const TerrainSyncRequest &request);
}
