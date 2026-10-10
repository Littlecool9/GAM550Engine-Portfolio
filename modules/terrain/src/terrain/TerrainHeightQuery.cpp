/**
 * @file TerrainHeightQuery.cpp
 * @author Jackey Cen
 * @brief Resident CPU height queries over one immutable terrain release.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainHeightQuery.h"

#include "PackInternal.h"

#include <terrain/TerrainSurface.h>
#include <terrain/TileAddressing.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Error;
        using detail::Hex;

        TerrainHeightResult Fail(TerrainHeightResult result, TerrainHeightFailure failure)
        {
            result.status = TerrainHeightStatus::Failed;
            result.failure = failure;
            result.localHeight = std::numeric_limits<double>::quiet_NaN();
            result.worldPoint.reset();
            result.spacing = std::numeric_limits<double>::quiet_NaN();
            result.maxError.reset();
            return result;
        }
    }

    bool TerrainHeightQuery::LayerState::Bakes(TileCoord tile) const
    {
        return info.layout.Contains(tile) && (!info.bakedTiles || baked.contains(tile));
    }

    const TerrainHeightQuery::Slot *TerrainHeightQuery::LayerState::Find(TileCoord tile) const
    {
        const auto it = slots.find(tile);
        return it == slots.end() ? nullptr : &it->second;
    }

    TerrainHeightQuery::TerrainHeightQuery(const TerrainReleaseInfo &release, std::vector<LayerState> layers)
        : mRelease(release), mLayers(std::move(layers))
    {
    }

    std::optional<TerrainHeightQuery> TerrainHeightQuery::Create(const TerrainReleaseInfo &release,
                                                                 std::vector<TerrainHeightLayer> layers)
    {
        if (release.asset.IsNull() || release.release.IsNull() || layers.empty() ||
            layers.size() > std::numeric_limits<uint32_t>::max())
            return std::nullopt;

        std::vector<LayerState> states;
        states.reserve(layers.size());
        bool everyTile = false;
        for (size_t l = 0; l < layers.size(); ++l)
        {
            TerrainHeightLayer &layer = layers[l];
            if (l > 0 && !TerrainLayersNest(states.back().info.layout, layer.layout))
                return std::nullopt;
            if (layer.maxDeviation && !(std::isfinite(*layer.maxDeviation) && *layer.maxDeviation >= 0.0))
                return std::nullopt;

            LayerState state{std::move(layer), {}, {}};
            if (state.info.bakedTiles)
            {
                for (const TileCoord tile : *state.info.bakedTiles)
                {
                    if (!state.info.layout.Contains(tile))
                        return std::nullopt;
                    state.baked.insert(tile);
                }
            }
            else
            {
                everyTile = true;
            }
            for (const auto &[tile, bound] : state.info.tileDeviations)
                if (!state.Bakes(tile) || !std::isfinite(bound) || bound < 0.0)
                    return std::nullopt;
            states.push_back(std::move(state));
        }

        // A map tile no layer bakes would have no source precision at all.
        if (!everyTile)
        {
            const TileRange range = states.front().info.layout.Tiles();
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                    if (std::none_of(states.begin(), states.end(),
                                     [tile = TileCoord{x, z}](const LayerState &s) { return s.Bakes(tile); }))
                        return std::nullopt;
        }
        return TerrainHeightQuery(release, std::move(states));
    }

    TerrainHeightQuery TerrainHeightQuery::Create(const TerrainReleaseHandle &release)
    {
        std::vector<LayerState> layers;
        for (const auto &layout : release.Layers())
            layers.push_back({TerrainHeightLayer{layout, std::vector<TileCoord>{}, std::nullopt}, {}, {}});
        for (const auto &r : release.Index().tiles)
        {
            if (r.kind != TerrainPayloadKind::Height) continue;
            layers[r.layer].info.bakedTiles->push_back(r.tile);
            layers[r.layer].baked.insert(r.tile);
            if (r.maxDeviation) layers[r.layer].info.tileDeviations.emplace(r.tile, *r.maxDeviation);
        }
        return TerrainHeightQuery(release.Info(), std::move(layers));
    }

    TerrainHeightResult TerrainHeightQuery::HeightAt(TerrainLocalPosition local, const TerrainHeightRequest &request)
    {
        TerrainHeightResult result;
        result.release = mRelease;
        if (request.release != mRelease)
            return Fail(result, TerrainHeightFailure::WrongRelease);
        const double required = request.requiredSpacing;
        if (!std::isfinite(required) || required <= 0.0)
            return Fail(result, TerrainHeightFailure::InvalidInput);

        const TileAddress address = AddressLocal(mLayers.front().info.layout, local);
        if (!address.IsInside())
        {
            result.status = TerrainHeightStatus::OutsideMap;
            return result;
        }
        const TileCoord tile = address.tile;
        result.tile = tile;

        std::optional<uint32_t> source;
        std::optional<uint32_t> target;
        std::optional<uint32_t> answer;
        for (uint32_t l = 0; l < mLayers.size(); ++l)
        {
            const LayerState &layer = mLayers[l];
            if (!layer.Bakes(tile))
                continue;
            if (!source)
                source = l;
            // Fine to coarse, so the last fit is the cheapest to load.
            if (layer.info.layout.SampleSpacing() <= required)
                target = l;
            const Slot *slot = layer.Find(tile);
            if (!answer && slot && slot->residency == TerrainTileResidency::Resident)
                answer = l;
        }
        if (!target)
            target = source;
        result.sourceSpacing = mLayers[*source].info.layout.SampleSpacing();

        const bool meets = answer && mLayers[*answer].info.layout.SampleSpacing() <= required;
        if (!meets)
        {
            const Slot *wanted = mLayers[*target].Find(tile);
            if (wanted && wanted->residency == TerrainTileResidency::Failed)
            {
                result.layer = *target;
                return Fail(result, TerrainHeightFailure::LoadFailed);
            }
            if (!wanted)
                Request(*target, tile);
            if (!answer)
            {
                result.status = TerrainHeightStatus::Loading;
                result.layer = *target;
                return result;
            }
        }

        const LayerState &layer = mLayers[*answer];
        Slot &slot = mLayers[*answer].slots.find(tile)->second;
        if (!slot.used)
        {
            slot.used = true;
            mUsed.push_back({mRelease.release, {mRelease.asset, tile}, *answer, layer.info.layout.SampleSpacing()});
        }
        const std::span<const float> samples = slot.heights->Samples();
        const SurfaceSample surface = EvaluateSurface(
            layer.info.layout, local,
            [&samples, tile](TileCoord owner) { return owner == tile ? samples : std::span<const float>{}; });
        result.layer = *answer;
        if (!surface.IsOk())
            return Fail(result, TerrainHeightFailure::InvalidSamples);

        result.localHeight = surface.height;
        result.spacing = layer.info.layout.SampleSpacing();
        if (meets)
            result.status = TerrainHeightStatus::MeetsRequirement;
        else if (result.sourceSpacing <= required)
            result.status = TerrainHeightStatus::CoarserWhileLoading;
        else
            result.status = TerrainHeightStatus::CoarserAtSourceLimit;

        // A coarser layer is only as good as its recorded deviation.
        const double tolerance = SurfaceTolerance(slot.maxAbsHeight);
        if (*answer == *source)
            result.maxError = tolerance;
        else if (const auto bound = layer.info.tileDeviations.find(tile); bound != layer.info.tileDeviations.end())
            result.maxError = bound->second + tolerance;
        else if (layer.info.maxDeviation)
            result.maxError = *layer.info.maxDeviation + tolerance;
        return result;
    }

    TerrainHeightResult TerrainHeightQuery::HeightAt(WorldPosition world, const TerrainPlacement &placement,
                                                     const TerrainHeightRequest &request)
    {
        TerrainHeightResult result;
        result.release = mRelease;
        if (placement.asset != mRelease.asset)
            return Fail(result, TerrainHeightFailure::WrongRelease);
        const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, placement.translation);
        if (!local)
            return Fail(result, TerrainHeightFailure::InvalidInput);

        result = HeightAt(*local, request);
        if (!result.HasHeight())
            return result;

        const glm::dvec3 meters = local->Meters();
        const std::optional<TerrainLocalPosition> surface =
            TerrainLocalPosition::FromMeters({meters.x, result.localHeight, meters.z});
        result.worldPoint = surface ? TryTerrainLocalToWorld(*surface, placement.translation) : std::nullopt;
        if (!result.worldPoint)
            return Fail(result, TerrainHeightFailure::InvalidInput);
        return result;
    }

    TerrainPackError TerrainHeightQuery::CheckSlot(const TerrainReleaseInfo &from, uint32_t layer,
                                                   TileCoord tile) const
    {
        if (from != mRelease)
            return Error(Code::WrongIdentity, "tile {} of asset {} release {} offered to the query of release {}",
                         Describe(tile), Hex(from.asset.value), Hex(from.release.value),
                         Hex(mRelease.release.value));
        if (layer >= mLayers.size())
            return Error(Code::InvalidInput, "layer {} does not exist (the query has {})", layer, mLayers.size());
        if (!mLayers[layer].Bakes(tile))
            return Error(Code::NotFound, "tile {} is not baked in layer {}", Describe(tile), layer);
        return {};
    }

    TerrainPackError TerrainHeightQuery::Insert(const TerrainReleaseInfo &from, uint32_t layer,
                                                std::shared_ptr<const TileHeights> tile)
    {
        if (!tile)
            return Error(Code::InvalidInput, "no tile");
        const TileCoord coord = tile->Tile();
        TerrainPackError error = CheckSlot(from, layer, coord);
        if (error.code != Code::None)
            return error;
        const int32_t n = mLayers[layer].info.layout.IntervalsPerTile();
        if (tile->IntervalsPerTile() != n)
            return Error(Code::InvalidInput, "tile {} has {} intervals, layer {} has {}", Describe(coord),
                         tile->IntervalsPerTile(), layer, n);

        double maxAbs = 0.0;
        for (const float h : tile->Samples())
        {
            if (!std::isfinite(h))
                return Error(Code::InvalidInput, "tile {} has a non-finite sample", Describe(coord));
            maxAbs = std::max(maxAbs, std::abs(double(h)));
        }

        Slot &slot = mLayers[layer].slots[coord];
        slot = {TerrainTileResidency::Resident, std::move(tile), maxAbs, {}};
        Unqueue(layer, coord);
        return {};
    }

    TerrainPackError TerrainHeightQuery::MarkFailed(const TerrainReleaseInfo &from, uint32_t layer, TileCoord tile,
                                                    TerrainPackError error)
    {
        TerrainPackError refused = CheckSlot(from, layer, tile);
        if (refused.code != Code::None)
            return refused;
        if (error.code == Code::None)
            error.code = Code::Io;
        mLayers[layer].slots[tile] = {TerrainTileResidency::Failed, nullptr, 0.0, std::move(error)};
        Unqueue(layer, tile);
        return {};
    }

    void TerrainHeightQuery::Evict(uint32_t layer, TileCoord tile)
    {
        if (layer >= mLayers.size())
            return;
        mLayers[layer].slots.erase(tile);
        Unqueue(layer, tile);
    }

    TerrainTileResidency TerrainHeightQuery::Residency(uint32_t layer, TileCoord tile) const
    {
        const Slot *slot = layer < mLayers.size() ? mLayers[layer].Find(tile) : nullptr;
        return slot ? slot->residency : TerrainTileResidency::Absent;
    }

    std::shared_ptr<const TileHeights> TerrainHeightQuery::ResidentTile(uint32_t layer, TileCoord tile) const
    {
        const Slot *slot = layer < mLayers.size() ? mLayers[layer].Find(tile) : nullptr;
        return slot ? slot->heights : nullptr;
    }

    const TerrainPackError *TerrainHeightQuery::TileError(uint32_t layer, TileCoord tile) const
    {
        const Slot *slot = layer < mLayers.size() ? mLayers[layer].Find(tile) : nullptr;
        return slot && slot->residency == TerrainTileResidency::Failed ? &slot->error : nullptr;
    }

    std::vector<TerrainPayloadRequest> TerrainHeightQuery::TakePayloadRequests()
    {
        return std::exchange(mQueue, {});
    }

    std::vector<TerrainPayloadRequest> TerrainHeightQuery::TakeUsedTiles()
    {
        for (const TerrainPayloadRequest &used : mUsed)
            if (const auto slot = mLayers[used.layer].slots.find(used.tile.coord); slot != mLayers[used.layer].slots.end())
                slot->second.used = false;
        return std::exchange(mUsed, {});
    }

    void TerrainHeightQuery::Request(uint32_t layer, TileCoord tile)
    {
        mLayers[layer].slots[tile].residency = TerrainTileResidency::Requested;
        const double spacing = mLayers[layer].info.layout.SampleSpacing();
        mQueue.push_back({mRelease.release, {mRelease.asset, tile}, layer, spacing});
    }

    void TerrainHeightQuery::Unqueue(uint32_t layer, TileCoord tile)
    {
        std::erase_if(mQueue, [&](const TerrainPayloadRequest &r) { return r.layer == layer && r.tile.coord == tile; });
    }

    TerrainPreloadReport PreloadTerrainTiles(TerrainHeightQuery &query, const TerrainReleaseHandle &release,
                                             std::span<const TileCoord> tiles, std::optional<uint32_t> releaseLayer)
    {
        TerrainPreloadReport report;
        const TerrainReleaseInfo &info = release.Info();
        if (releaseLayer && *releaseLayer >= release.Layers().size())
        {
            report.firstError = Error(Code::InvalidInput, "unknown release layer {}", *releaseLayer);
            return report;
        }
        if (info != query.Release())
        {
            report.firstError = Error(Code::WrongIdentity, "release {} cannot fill the query of release {}",
                                      Hex(info.release.value), Hex(query.Release().release.value));
            return report;
        }

        std::unordered_set<TileCoord> visited;
        for (TileCoord tile : tiles)
        {
            if (!visited.insert(tile).second) continue;
            if (!release.Index().Find(tile))
            {
                if (++report.failed == 1)
                    report.firstError = Error(Code::NotFound, "tile {} is outside the release", Describe(tile));
                continue;
            }
            const uint32_t first = releaseLayer.value_or(0);
            const uint32_t end = releaseLayer ? first + 1 : static_cast<uint32_t>(release.Layers().size());
            for (uint32_t r = first; r < end; ++r)
            {
                const auto *record = release.Index().Find(tile, r);
                if (!record)
                {
                    if (releaseLayer && ++report.failed == 1)
                        report.firstError = Error(Code::NotFound, "tile {} has no Height payload in release layer {}",
                                                  Describe(tile), r);
                    continue;
                }
                std::optional<uint32_t> layer;
                for (uint32_t l = 0; l < query.LayerCount(); ++l)
                    if (query.Layer(l).layout.SampleSpacing() == record->sampleSpacing) layer = l;
                if (!layer)
                {
                    report.firstError = Error(Code::InvalidInput, "the query has no layer of the release's {} m spacing",
                                              record->sampleSpacing);
                    return report;
                }
                auto read = release.ReadTileRecord(*record);
                TerrainPackError error = read.error;
                if (read) error = query.Insert(info, *layer, std::make_shared<const TileHeights>(std::move(*read.value)));
                else (void)query.MarkFailed(info, *layer, tile, read.error);
                if (error.code == Code::None) ++report.loaded;
                else if (++report.failed == 1) report.firstError = std::move(error);
            }
        }
        return report;
    }

    TerrainPreloadReport PreloadAllTerrainTiles(TerrainHeightQuery &query, const TerrainReleaseHandle &release,
                                                std::optional<uint32_t> releaseLayer)
    {
        TerrainPreloadReport total;
        if (releaseLayer && *releaseLayer >= release.Layers().size())
        {
            total.firstError = Error(Code::InvalidInput, "unknown release layer {}", *releaseLayer);
            return total;
        }
        if (release.Info() != query.Release())
        {
            total.firstError = Error(Code::WrongIdentity, "release identity differs from the query");
            return total;
        }
        for (const auto &r : release.Index().tiles)
        {
            if ((releaseLayer && r.layer != *releaseLayer) || r.kind != TerrainPayloadKind::Height) continue;
            std::optional<uint32_t> layer;
            for (uint32_t l = 0; l < query.LayerCount(); ++l)
                if (query.Layer(l).layout.SampleSpacing() == r.sampleSpacing) layer = l;
            if (!layer)
            {
                total.firstError = Error(Code::InvalidInput, "query has no {} m layer", r.sampleSpacing);
                return total;
            }
            auto read = release.ReadTileRecord(r);
            auto error = read.error;
            if (read) error = query.Insert(release.Info(), *layer, std::make_shared<const TileHeights>(std::move(*read.value)));
            else (void)query.MarkFailed(release.Info(), *layer, r.tile, read.error);
            if (error.code == Code::None) ++total.loaded;
            else if (++total.failed == 1) total.firstError = error;
        }
        return total;
    }
}
