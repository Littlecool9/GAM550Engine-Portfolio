/**
 * @file TerrainCandidate.h
 * @author Jackey Cen
 * @brief The candidate directory a bake writes: release files plus a manifest.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainReport.h"

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainPackWriter.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    // Written last. Publishing copies only the release files; the manifest stays behind.
    inline constexpr std::string_view kCandidateManifestFileName = "candidate.json";
    inline constexpr uint32_t kCandidateManifestVersion = 3;

    struct TerrainCandidateManifest
    {
        uint32_t toolVersion = kTerrainToolVersion;
        std::string assetPath;
        // For people, not identity.
        std::string recipePath;
        TerrainReleaseInfo release;
        uint64_t nonce = 0;
        uint64_t heightsHash = 0;
        uint32_t formatMinor = 0;
        uint32_t contentVersion = 0;
        uint32_t surfaceVersion = 0;
        uint32_t layerCount = 0;
        uint32_t payloadCount = 0;
        // Index first, then packs in index order.
        std::vector<TerrainFileSummary> files;
        std::optional<TerrainSeamSummary> seams;
    };

    struct ManifestIssue
    {
        std::string field;
        std::string message;
    };

    struct ManifestParseResult
    {
        std::optional<TerrainCandidateManifest> manifest;
        std::vector<ManifestIssue> issues;
    };

    std::vector<TerrainFileSummary> DescribeReleaseFiles(const TerrainEncodedRelease &encoded);

    std::string EncodeCandidateManifest(const TerrainCandidateManifest &manifest);
    ManifestParseResult ParseCandidateManifest(std::string_view json);

    // dir must exist. Plain writes: the caller renames the directory into place.
    bool WriteCandidateFiles(const std::filesystem::path &dir, const TerrainEncodedRelease &encoded,
                             const TerrainCandidateManifest &manifest, std::string &error);
}
