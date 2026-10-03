/**
 * @file TerrainPackage.h
 * @author Jackey Cen
 * @brief Copies an asset's current release into the game directory a build without the editor reads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainPublish.h"
#include "TerrainReport.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace mir::terrainbake
{
    struct TerrainPackageRequest
    {
        std::string idRoot;
        std::string sourcePath;
        // Only read.
        std::filesystem::path gameDir;
        // Its TerrainReleasesRoot is written.
        std::filesystem::path outDir;
        std::chrono::milliseconds lockWait{std::chrono::minutes(10)};
        std::optional<TerrainPublishFault> fault;
    };

    TerrainReport RunTerrainPackage(const TerrainPackageRequest &request);
}
