/**
 * @file TerrainPublish.h
 * @author Jackey Cen
 * @brief Publishes a validated candidate as an immutable release and switches the current pointer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRecipe.h"
#include "TerrainReport.h"

#include <assets/AssetId.h>
#include <terrain/TerrainIdentity.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir::terrainbake
{
    // Only an open handle locks; the file means nothing by itself.
    inline constexpr std::string_view kTerrainPublishLockFileName = ".publish.lock";

    // The OS drops it when the process dies; readers never take it.
    class TerrainPublishLock
    {
    public:
        // Retries until wait runs out; empty, with the reason in error, if busy.
        [[nodiscard]] static std::optional<TerrainPublishLock> Acquire(const std::filesystem::path &assetDir,
                                                                       std::chrono::milliseconds wait,
                                                                       std::string &error);

        TerrainPublishLock(TerrainPublishLock &&other) noexcept;
        TerrainPublishLock &operator=(TerrainPublishLock &&other) noexcept;
        TerrainPublishLock(const TerrainPublishLock &) = delete;
        TerrainPublishLock &operator=(const TerrainPublishLock &) = delete;
        ~TerrainPublishLock();

        const std::filesystem::path &AssetDir() const { return mAssetDir; }

    private:
        TerrainPublishLock(std::filesystem::path assetDir, intptr_t handle);
        void Release();

        std::filesystem::path mAssetDir;
        intptr_t mHandle = -1; // a HANDLE on Windows, a file descriptor elsewhere
    };

    enum class TerrainPublishStep : uint8_t
    {
        Candidate, // before the release tree is touched
        Copy,      // after part of the copy reached .partial
        Verify,    // fail: flips a byte of the copy
        Rename,    // before the .partial directory takes the release's name
        Pointer,   // after current.tmp is written, before it replaces current
    };

    // Tests only. An interrupt returns at once with no cleanup, as a killed process would.
    struct TerrainPublishFault
    {
        TerrainPublishStep step = TerrainPublishStep::Candidate;
        bool interrupt = false;
    };

    struct TerrainPublishRequest
    {
        std::filesystem::path candidateDir;
        std::filesystem::path releasesRoot;
        // Empty: no recipe check.
        std::filesystem::path recipePath;
        // Zero fails at once if another publisher holds the asset.
        std::chrono::milliseconds lockWait{0};
        std::optional<TerrainPublishFault> fault;
    };

    bool PublishTerrainCandidate(const TerrainPublishRequest &request, const TerrainRecipe *recipe,
                                 TerrainPublishLock *heldLock, TerrainReport &report);

    TerrainReport RunTerrainPublish(const TerrainPublishRequest &request);

    // No log when absent; problem says why it is empty.
    std::optional<TerrainReleaseId> PeekTerrainCurrent(const std::filesystem::path &releasesRoot, AssetId asset,
                                                       std::string *problem = nullptr);

    // Compared as absolute, lexically normal paths.
    bool IsSameOrInside(const std::filesystem::path &a, const std::filesystem::path &b);
}
