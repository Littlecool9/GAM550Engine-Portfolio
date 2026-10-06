/**
 * @file TerrainHeightQuery.h
 * @author Jackey Cen
 * @brief Resident CPU height queries over one immutable terrain release.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    // One sample spacing of a release's final heights; P1 has exactly one.
    struct TerrainHeightLayer
    {
        TerrainLayout layout;
        // nullopt means every layout tile.
        std::optional<std::vector<TileCoord>> bakedTiles;
        // Measured bound against the finest baked surface; nullopt if unmeasured.
        std::optional<double> maxDeviation;
        std::unordered_map<TileCoord, double> tileDeviations;
    };

    struct TerrainHeightRequest
    {
        // A service bound to another release answers WrongRelease.
        TerrainReleaseInfo release;
        // Coarsest acceptable spacing in meters.
        double requiredSpacing = 0.0;
    };

    enum class TerrainHeightStatus : uint8_t
    {
        MeetsRequirement,
        // A finer payload is requested.
        CoarserWhileLoading,
        // Nothing finer is baked here, so waiting never meets the requirement.
        CoarserAtSourceLimit,
        Loading,
        OutsideMap,
        Failed,
    };

    enum class TerrainHeightFailure : uint8_t
    {
        None,
        // Also when the placement names another asset.
        WrongRelease,
        // Bad spacing, or a world/local conversion that overflows.
        InvalidInput,
        // See TileError.
        LoadFailed,
        InvalidSamples,
    };

    // Heights are NaN unless HasHeight(), so a skipped status check never reads zero.
    struct TerrainHeightResult
    {
        TerrainHeightStatus status = TerrainHeightStatus::Failed;
        TerrainHeightFailure failure = TerrainHeightFailure::None;
        double localHeight = std::numeric_limits<double>::quiet_NaN();
        // World queries only.
        std::optional<WorldPosition> worldPoint;
        double spacing = std::numeric_limits<double>::quiet_NaN();
        // Finest spacing baked at this point.
        double sourceSpacing = std::numeric_limits<double>::quiet_NaN();
        // Bound against the release's finest baked surface, never the recipe.
        std::optional<double> maxError;
        TerrainReleaseInfo release;
        TileCoord tile;
        uint32_t layer = 0;

        bool HasHeight() const
        {
            return status == TerrainHeightStatus::MeetsRequirement ||
                   status == TerrainHeightStatus::CoarserWhileLoading ||
                   status == TerrainHeightStatus::CoarserAtSourceLimit;
        }
    };

    enum class TerrainTileResidency : uint8_t
    {
        Absent,
        // Stays Requested after TakePayloadRequests until Insert or MarkFailed.
        Requested,
        Resident,
        Failed,
    };

    struct TerrainPayloadRequest
    {
        TerrainReleaseId release;
        TileKey tile;
        uint32_t layer = 0;
        double spacing = 0.0;

        bool operator==(const TerrainPayloadRequest &) const = default;
    };

    // Never reads files; not thread-safe.
    class TerrainHeightQuery
    {
    public:
        // Layers run finest to coarsest; each spacing is an integer multiple of the previous.
        [[nodiscard]] static std::optional<TerrainHeightQuery> Create(const TerrainReleaseInfo &release,
                                                                      std::vector<TerrainHeightLayer> layers);

        // Indexed sparse layers. Reads nothing.
        [[nodiscard]] static TerrainHeightQuery Create(const TerrainReleaseHandle &release);

        const TerrainReleaseInfo &Release() const { return mRelease; }
        size_t LayerCount() const { return mLayers.size(); }
        const TerrainHeightLayer &Layer(uint32_t layer) const { return mLayers[layer].info; }

        // Answers from the finest resident layer; requests a finer one when that is too coarse.
        TerrainHeightResult HeightAt(TerrainLocalPosition local, const TerrainHeightRequest &request);

        TerrainHeightResult HeightAt(WorldPosition world, const TerrainPlacement &placement,
                                     const TerrainHeightRequest &request);

        // Replaces whatever the slot held.
        TerrainPackError Insert(const TerrainReleaseInfo &from, uint32_t layer,
                                std::shared_ptr<const TileHeights> tile);

        // Queries needing this tile then fail with LoadFailed instead of waiting.
        TerrainPackError MarkFailed(const TerrainReleaseInfo &from, uint32_t layer, TileCoord tile,
                                    TerrainPackError error);

        TerrainTileResidency Residency(uint32_t layer, TileCoord tile) const;
        // Null unless resident.
        std::shared_ptr<const TileHeights> ResidentTile(uint32_t layer, TileCoord tile) const;
        // Null unless Failed.
        const TerrainPackError *TileError(uint32_t layer, TileCoord tile) const;

        // Oldest first, without duplicates.
        std::span<const TerrainPayloadRequest> PayloadRequests() const { return mQueue; }
        std::vector<TerrainPayloadRequest> TakePayloadRequests();

    private:
        struct Slot
        {
            TerrainTileResidency residency = TerrainTileResidency::Absent;
            std::shared_ptr<const TileHeights> heights;
            double maxAbsHeight = 0.0;
            TerrainPackError error;
        };

        struct LayerState
        {
            TerrainHeightLayer info;
            // Empty when info.bakedTiles is nullopt.
            std::unordered_set<TileCoord> baked;
            std::unordered_map<TileCoord, Slot> slots;

            bool Bakes(TileCoord tile) const;
            const Slot *Find(TileCoord tile) const;
        };

        TerrainHeightQuery(const TerrainReleaseInfo &release, std::vector<LayerState> layers);

        TerrainPackError CheckSlot(const TerrainReleaseInfo &from, uint32_t layer, TileCoord tile) const;
        void Request(uint32_t layer, TileCoord tile);
        void Unqueue(uint32_t layer, TileCoord tile);

        TerrainReleaseInfo mRelease;
        std::vector<LayerState> mLayers;
        std::vector<TerrainPayloadRequest> mQueue;
    };

    struct TerrainPreloadReport
    {
        size_t loaded = 0;
        size_t failed = 0;
        // The first failure, or the refusal of the whole call.
        TerrainPackError firstError;

        bool Ok() const { return firstError.code == TerrainPackErrorCode::None; }
    };

    // Synchronous; selected tiles use indexed keys and duplicate requests read once.
    // A missing selected-layer payload is NotFound without marking a residency slot.
    // All-layer loading skips unbaked sparse keys. Another release is refused before IO.
    TerrainPreloadReport PreloadTerrainTiles(TerrainHeightQuery &query, const TerrainReleaseHandle &release,
                                             std::span<const TileCoord> tiles, std::optional<uint32_t> releaseLayer = std::nullopt);
    TerrainPreloadReport PreloadAllTerrainTiles(TerrainHeightQuery &query, const TerrainReleaseHandle &release,
                                                std::optional<uint32_t> releaseLayer = std::nullopt);
}
