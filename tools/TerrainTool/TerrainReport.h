/**
 * @file TerrainReport.h
 * @author Jackey Cen
 * @brief Checks, diagnostics and the JSON report of a TerrainTool bake or validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileKey.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir::terrainbake
{
    // Bump when a command's behaviour or output changes.
    inline constexpr uint32_t kTerrainToolVersion = 1;
    inline constexpr uint32_t kTerrainReportVersion = 1;

    // In report order.
    namespace check
    {
        inline constexpr std::string_view kArguments = "arguments";
        inline constexpr std::string_view kRecipe = "recipe";
        inline constexpr std::string_view kGenerate = "generate";
        inline constexpr std::string_view kEncode = "encode";
        inline constexpr std::string_view kWrite = "write";
        inline constexpr std::string_view kCandidate = "candidate";
        inline constexpr std::string_view kFormat = "format";
        inline constexpr std::string_view kIndex = "index";
        inline constexpr std::string_view kPacks = "packs";
        inline constexpr std::string_view kCoverage = "coverage";
        inline constexpr std::string_view kPayloads = "payloads";
        inline constexpr std::string_view kFinite = "finite";
        inline constexpr std::string_view kEdges = "edges";
        inline constexpr std::string_view kCorners = "corners";
        inline constexpr std::string_view kIdentity = "identity";
    }

    enum class CheckStatus : uint8_t
    {
        Passed,
        Failed,
        Skipped, // not run, or cut short by an earlier failure
    };

    // Every diagnostic fails its check. field is a recipe or manifest path, a file name or a data field.
    struct TerrainDiagnostic
    {
        std::string check;
        std::string field;
        std::vector<TileCoord> tiles;
        std::optional<GlobalSampleIndex> sample;
        std::string message;
    };

    struct TerrainCheck
    {
        std::string name;
        CheckStatus status = CheckStatus::Skipped;
        std::string detail;
    };

    struct TerrainTileSummary
    {
        TileCoord tile;
        std::string pack;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        bool readable = false;
    };

    struct TerrainFileSummary
    {
        std::string name;
        uint64_t bytes = 0;
        uint64_t checksum = 0; // Fnv1a64 of the whole file
    };

    class TerrainReport
    {
    public:
        TerrainReport(std::string command, std::initializer_list<std::string_view> checks);

        void Ran(std::string_view check, std::string detail = {});
        void Skip(std::string_view check, std::string reason);
        void Add(TerrainDiagnostic diagnostic);

        CheckStatus Status(std::string_view check) const;
        bool Ok() const;

        std::string command;
        std::filesystem::path recipePath;
        std::optional<TerrainSourceFingerprint> recipeFingerprint;
        std::filesystem::path candidateDir;
        std::string assetPath;
        std::optional<TerrainReleaseInfo> release;
        uint32_t formatMinor = 0;
        uint32_t contentVersion = 0;
        uint32_t surfaceVersion = 0;
        uint64_t heightsHash = 0;
        std::optional<uint64_t> nonce;
        std::optional<TerrainLayout> layout;
        std::vector<TerrainTileSummary> tiles;
        std::vector<TerrainFileSummary> files;
        std::vector<TerrainCheck> checks;
        std::vector<TerrainDiagnostic> diagnostics;

    private:
        TerrainCheck *Find(std::string_view check);
    };

    std::string CheckStatusName(CheckStatus status);

    // "[edges] tiles (-1, -1) (0, -1) sample (0, -54) samples: ..."
    std::string FormatDiagnostic(const TerrainDiagnostic &diagnostic);

    std::string TerrainReportJson(const TerrainReport &report);

    // Creates the parent directory.
    bool WriteTerrainReport(const std::filesystem::path &path, const TerrainReport &report, std::string &error);
}
