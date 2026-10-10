/**
 * @file TerrainStreamingSettings.h
 * @author Jackey Cen
 * @brief Terrain streaming budgets, IO and retry policy, read from data files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // Shipped next to a game's game.json.
    inline constexpr std::string_view kTerrainStreamingSettingsFileName = "terrain_streaming.json";
    // Machine-local, next to the executable; overrides the game's file key by key.
    inline constexpr std::string_view kTerrainStreamingLocalSettingsFileName = "terrain_streaming.local.json";

    inline constexpr uint32_t kMaxTerrainStreamingWorkers = 64;

    enum class TerrainResidencyMode : uint8_t
    {
        // Eager when every indexed payload fits what the CPU budget has left, else Interest.
        Auto,
        // Every payload requested when the release is added, through the same asynchronous path.
        Eager,
        // Only what interests and height queries ask for.
        Interest,
    };

    std::string_view TerrainResidencyModeName(TerrainResidencyMode mode);

    // Every field is required. The initial values fail validation, so nothing falls back to a code default.
    struct TerrainStreamingSettings
    {
        struct Cpu
        {
            // Decoded payload bytes held or being read.
            uint64_t budgetBytes = 0;
        } cpu;

        struct Io
        {
            uint32_t workerThreads = 0;
            // Finished reads installed by one Update; the rest wait for the next.
            uint32_t maxInstallsPerFrame = 0;
        } io;

        // An IO failure is retried after firstDelaySeconds, doubling up to maxDelaySeconds, until maxAttempts
        // reads have failed. A failed payload someone still wants starts over after failedHoldSeconds.
        struct Retry
        {
            uint32_t maxAttempts = 0;
            double firstDelaySeconds = std::numeric_limits<double>::quiet_NaN();
            double maxDelaySeconds = std::numeric_limits<double>::quiet_NaN();
            double failedHoldSeconds = std::numeric_limits<double>::quiet_NaN();
        } retry;

        struct Residency
        {
            std::optional<TerrainResidencyMode> mode;
            // A height query's tile stays wanted this long after it was last asked for or answered from.
            double queryLeaseSeconds = std::numeric_limits<double>::quiet_NaN();
        } residency;
    };

    // One "<field>: <problem>" line per invalid field, named by its JSON path; empty when valid.
    std::vector<std::string> ValidateTerrainStreamingSettings(const TerrainStreamingSettings &settings);

    struct TerrainStreamingSettingsLoad
    {
        std::optional<TerrainStreamingSettings> settings;
        // Every missing, unknown or invalid key, each naming the file it is (or belongs) in.
        std::vector<std::string> problems;
        bool gameFileFound = false;
        bool localFileFound = false;
    };

    // The game file is required; the local file is optional and replaces values key by key.
    TerrainStreamingSettingsLoad LoadTerrainStreamingSettings(const std::filesystem::path &gameFile,
                                                              const std::filesystem::path &localFile);
}
