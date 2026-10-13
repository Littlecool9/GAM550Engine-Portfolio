/**
 * @file TerrainStreamer.h
 * @author Jackey Cen
 * @brief Schedules asynchronous payload range reads and keeps decoded payloads within a CPU budget.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLod.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace mir
{
    class TerrainResidentRelease;

    // One payload of one published release: what requests and the CPU cache are keyed by.
    struct TerrainPayloadKey
    {
        AssetId asset;
        TerrainReleaseId release;
        TerrainPayloadKind kind = TerrainPayloadKind::Height;
        uint32_t level = 0;
        uint32_t layer = 0;
        // A tile, or a Region's x, z at its level.
        TileCoord coord;

        bool operator==(const TerrainPayloadKey &) const = default;
    };

    TerrainPayloadKey TerrainPayloadKeyOf(const TerrainReleaseInfo &release, const TerrainTileRecord &record);
    std::string DescribeTerrainPayloadKey(const TerrainPayloadKey &key);

    // Bytes the decoded payload occupies, known from its index record before any read.
    uint64_t TerrainPayloadFootprint(const TerrainTileRecord &record);

    // Higher is read first and evicted last.
    enum class TerrainDemand : uint8_t
    {
        Prefetch,
        Visible,
        Query,
        // Collision, and everything of an Eager release; may exceed the budget, which Stats reports.
        Required,
    };

    struct TerrainPayloadWant
    {
        TerrainPayloadKey key;
        TerrainDemand demand = TerrainDemand::Prefetch;
        // Within one demand, lower is read sooner and evicted later.
        float rank = 0.0f;
    };

    enum class TerrainPayloadState : uint8_t
    {
        // Not wanted, or canceled or evicted.
        Absent,
        // Wanted; held back by the budget or a retry delay.
        Waiting,
        // With a worker: range read, record checksum, decode.
        Loading,
        // Decoded and installed; CPU-queryable.
        Ready,
        // Out of attempts, corrupt, or not in the index; see error.
        Failed,
    };

    struct TerrainPayloadStatus
    {
        TerrainPayloadState state = TerrainPayloadState::Absent;
        // Failed reads in the current round.
        uint32_t attempts = 0;
        TerrainPackError error;
    };

    struct TerrainStreamingStats
    {
        uint64_t budgetBytes = 0;
        uint64_t residentBytes = 0;
        // Reserved for reads not installed yet.
        uint64_t loadingBytes = 0;
        // Highest residentBytes + loadingBytes at the end of an Update.
        uint64_t peakBytes = 0;
        // Past the budget; only Required payloads get there.
        uint64_t overBudgetBytes = 0;
        size_t waiting = 0;
        size_t loading = 0;
        size_t ready = 0;
        size_t failed = 0;
        uint64_t readsStarted = 0;
        uint64_t readsSucceeded = 0;
        uint64_t readsFailed = 0;
        uint64_t retries = 0;
        // Wants dropped before their read started.
        uint64_t canceled = 0;
        uint64_t evicted = 0;
        // Finished reads whose entry or release was gone; never installed.
        uint64_t discarded = 0;
        uint64_t bytesRead = 0;
        // Worker time, summed.
        double readSeconds = 0.0;
        double decodeSeconds = 0.0;
        // Main-thread time spent in Update.
        double lastUpdateSeconds = 0.0;
        double maxUpdateSeconds = 0.0;
    };

    // One release's share of a residency: what a release switch waits for, and what an old release still holds.
    struct TerrainReleaseUsage
    {
        // Wanted, and held back by a budget.
        size_t waiting = 0;
        // Being read, retried, built or uploaded.
        size_t inFlight = 0;
        size_t ready = 0;
        size_t failed = 0;
        // Counted in the residency's budget.
        uint64_t bytes = 0;
        // The first failure's message.
        std::string failure;
    };

    using TerrainInterestId = uint32_t;

    // Main-thread object. Requests for one key merge across interests, Eager releases and height queries
    // into one read and one shared payload; only the jobs it posts touch byte sources.
    class TerrainStreamer
    {
    public:
        // Monotonic seconds; the default counts from construction.
        using Clock = std::function<double()>;

        // Settings must validate. WaitForEager needs an executor that runs jobs on its own.
        TerrainStreamer(const TerrainStreamingSettings &settings, std::shared_ptr<TerrainExecutor> executor,
                        Clock clock = {});
        ~TerrainStreamer();

        TerrainStreamer(const TerrainStreamer &) = delete;
        TerrainStreamer &operator=(const TerrainStreamer &) = delete;

        const TerrainStreamingSettings &Settings() const;

        // Held weakly: once nobody else holds a release, its payloads go at the next Update. Returns the
        // mode chosen when the release was first added. A staged release is one a release switch prepares: its
        // mode is chosen as if the other releases of its asset were gone, and an Eager one's payloads are only
        // prefetched until it is added again unstaged.
        TerrainResidencyMode Add(const std::shared_ptr<TerrainResidentRelease> &release, bool staged = false);
        std::shared_ptr<TerrainResidentRelease> Find(AssetId asset, TerrainReleaseId release) const;
        std::optional<TerrainResidencyMode> ModeOf(const TerrainReleaseInfo &release) const;

        TerrainInterestId AddInterest();
        // Replaces what the interest wanted; read at the next Update. Keys of releases not added are ignored.
        void SetInterest(TerrainInterestId interest, std::span<const TerrainPayloadWant> wants);
        void RemoveInterest(TerrainInterestId interest);

        // The one install point per frame: takes height-query requests, installs finished reads, cancels,
        // retries, evicts and starts reads. Never reads a file or waits.
        void Update();

        // Stop-gap for consumers that need a whole release: updates until every payload of an Eager
        // release is Ready (no error) or one has Failed (its error). Blocks the calling thread.
        TerrainPackError WaitForEager(const TerrainReleaseInfo &release);
        // True once a finished read waits for Update.
        bool WaitForCompletion(std::chrono::duration<double> timeout);

        TerrainPayloadStatus Status(const TerrainPayloadKey &key) const;
        // Null unless Ready.
        std::shared_ptr<const TileHeights> Heights(const TerrainPayloadKey &key) const;
        std::shared_ptr<const TerrainLodSurface> Lod(const TerrainPayloadKey &key) const;
        std::shared_ptr<const TerrainLodNormals> Normals(const TerrainPayloadKey &key) const;
        std::shared_ptr<const TerrainRegionMesh> Region(const TerrainPayloadKey &key) const;

        // Failed payloads still wanted start a new round of attempts at the next Update.
        void RetryFailed();

        TerrainStreamingStats Stats() const;
        TerrainReleaseUsage Usage(const TerrainReleaseInfo &release) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

template <>
struct std::hash<mir::TerrainPayloadKey>
{
    size_t operator()(const mir::TerrainPayloadKey &key) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(key.asset.value);
        const auto mix = [&h](uint64_t v) { h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
        mix(key.release.value);
        mix((uint64_t(uint32_t(key.kind)) << 32) | key.level);
        mix(key.layer);
        mix(std::hash<mir::TileCoord>{}(key.coord));
        return h;
    }
};
