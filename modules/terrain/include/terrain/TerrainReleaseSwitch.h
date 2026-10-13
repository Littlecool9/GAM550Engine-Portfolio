/**
 * @file TerrainReleaseSwitch.h
 * @author Jackey Cen
 * @brief Moves a loaded terrain asset to the release current names once that release is verified and warm.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainScene.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class TerrainCollisionResidency;
    class TerrainMeshResidency;
    class TerrainStreamer;

    // Reports in a row with nothing of a prewarm in flight that settle it; what the render phase asks for starts
    // at the next update.
    inline constexpr uint32_t kTerrainSwitchIdleReports = 2;

    enum class TerrainSwitchState : uint8_t
    {
        // Its index, then each pack, is read whole and checked on a worker, one at a time.
        Verifying,
        // Verified; its working set loads beside the loaded release's.
        Prewarming,
        // Switched to at the next Edit frame boundary.
        Ready,
        // Not usable; the loaded release stays until current moves again.
        Failed,
    };

    std::string_view TerrainSwitchStateName(TerrainSwitchState state);

    // A staged release's working set this update, as the caller measured it.
    struct TerrainPrewarmReport
    {
        // Everything its views would draw and its sources would collide with is usable.
        bool warm = false;
        // Some of it is being read, built or uploaded.
        bool inFlight = false;
        // Empty unless part of it failed.
        std::string failure;
    };

    // A staged release's prewarm as the residencies hold it. viewsWarm: every view's selection of it draws
    // only the units it chose, all with usable meshes.
    TerrainPrewarmReport MeasureTerrainPrewarm(const TerrainReleaseInfo &release, const TerrainStreamer &streamer,
                                               const TerrainMeshResidency *meshes,
                                               const TerrainCollisionResidency *collision, bool viewsWarm);

    // One asset's move from its loaded release to the one current names.
    struct TerrainReleaseCandidate
    {
        AssetId asset;
        std::string logicalPath;
        TerrainReleaseId loaded;
        // Null when current cannot be read.
        TerrainReleaseId release;
        TerrainSwitchState state = TerrainSwitchState::Verifying;
        // Known once the index is read.
        size_t packs = 0;
        size_t packsVerified = 0;
        uint64_t bytesVerified = 0;
        // Worker time.
        double verifySeconds = 0.0;
        // Clock times; NaN until reached.
        double startedAt = 0.0;
        double verifiedAt = std::numeric_limits<double>::quiet_NaN();
        double readyAt = std::numeric_limits<double>::quiet_NaN();
        // Ready with its whole working set usable, not settled by a budget.
        bool warm = false;
        std::string failure;
        // The asset's release directories when the index was read; nothing here deletes any.
        std::vector<TerrainStoredRelease> stored;
    };

    struct TerrainSwitchEvent
    {
        enum class Kind : uint8_t
        {
            Started,
            Verified,
            // current moved on, or the asset left the scene, before the switch.
            Dropped,
            Failed,
            Ready,
            // current of a loaded asset cannot be read; the loaded release stays.
            Unreadable,
        };

        Kind kind = Kind::Started;
        TerrainReleaseCandidate candidate;
    };

    struct TerrainSwitchStats
    {
        uint64_t jobs = 0;
        uint64_t bytesVerified = 0;
        // Verification results whose candidate was gone; never installed.
        uint64_t discarded = 0;
        uint64_t switched = 0;
        uint64_t failed = 0;
    };

    struct TerrainReleaseAdoption
    {
        TerrainReleaseCandidate candidate;
        std::shared_ptr<TerrainResidentRelease> release;
    };

    // Main-thread object for Edit. When current names a newer release B of a loaded asset, B is opened and every
    // pack checked on a worker; then B is added to the streamer staged, so its working set loads beside A's, and
    // once it is warm (or a budget keeps it from getting warmer) it is handed out at an Edit frame boundary. A
    // failed B is reported once and never read again until current moves; A stays in use throughout.
    class TerrainReleaseSwitch
    {
    public:
        using Clock = std::function<double()>;

        // open runs on the executor's threads; null opens from releasesRoot. The default clock counts from
        // construction.
        TerrainReleaseSwitch(std::shared_ptr<TerrainExecutor> executor, std::filesystem::path releasesRoot,
                             TerrainReleaseOpener open = {}, Clock clock = {});
        ~TerrainReleaseSwitch();

        TerrainReleaseSwitch(const TerrainReleaseSwitch &) = delete;
        TerrainReleaseSwitch &operator=(const TerrainReleaseSwitch &) = delete;

        // Reads the current pointers and nothing else. A loaded asset whose current names another release gets a
        // candidate for it, replacing one for an older release. True when an asset with no loaded release has a
        // moved current, so the scene should be resolved again.
        bool Poll(const TerrainWorld &world);

        // The install point: verification results; unless suspended, a verified candidate's release is added to
        // the streamer staged and the next check is posted.
        void Update(TerrainStreamer &streamer);

        // Releases whose working set loads ahead of their switch; a change bumps StagingVersion.
        std::vector<std::shared_ptr<TerrainResidentRelease>> Staged() const;
        uint64_t StagingVersion() const;

        // Settles a prewarming candidate: Failed on a failure, Ready when warm or after kTerrainSwitchIdleReports
        // reports in a row with nothing in flight.
        void Report(AssetId asset, const TerrainPrewarmReport &report);

        // Ready candidates leave with their releases, for ResolveTerrainWorld's adopt at an Edit frame boundary.
        std::vector<TerrainReleaseAdoption> TakeReady();

        // Play: no check starts and staged releases are let go; verified candidates keep their result.
        void Suspend();
        void Resume();
        bool Suspended() const;

        void Clear();

        std::vector<TerrainReleaseCandidate> Candidates() const;
        std::vector<TerrainSwitchEvent> TakeEvents();
        TerrainSwitchStats Stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
