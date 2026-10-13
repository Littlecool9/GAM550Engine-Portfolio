/**
 * @file TerrainCollisionResidency.cpp
 * @author Jackey Cen
 * @brief Keeps the height field pieces that collision sources need, built on workers and placed between steps.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainCollisionResidency.h"

#include "PackInternal.h"

#include <terrain/TerrainLayout.h>
#include <terrain/TerrainTelemetry.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace mir
{
    namespace
    {
        using SteadyClock = std::chrono::steady_clock;
        using detail::Hex;

        double Seconds(SteadyClock::duration duration)
        {
            return std::chrono::duration<double>(duration).count();
        }

        // Covers Jolt's speculative contact distance around a footprint.
        constexpr double kContactSlack = 0.1;
        constexpr size_t kLatencySamples = 16;
        constexpr double kInfinity = std::numeric_limits<double>::infinity();

        struct PieceKey
        {
            AssetId asset;
            TerrainReleaseId release;
            int32_t x = 0;
            int32_t z = 0;
            bool whole = false;
            bool coarse = false;

            bool operator==(const PieceKey &) const = default;
        };

        struct PieceKeyHash
        {
            size_t operator()(const PieceKey &key) const noexcept
            {
                size_t h = std::hash<uint64_t>{}(key.asset.value);
                const auto mix = [&h](uint64_t v) { h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
                mix(key.release.value);
                mix((uint64_t(uint32_t(key.x)) << 32) | uint32_t(key.z));
                mix(uint64_t(key.whole) << 1 | uint64_t(key.coarse));
                return h;
            }
        };

        TerrainCollisionPieceName NameOf(const PieceKey &key)
        {
            return {key.asset, key.release, key.x, key.z, key.whole, key.coarse};
        }

        bool SameRelease(const TerrainReleaseInfo &info, const PieceKey &key)
        {
            return info.asset == key.asset && info.release == key.release;
        }

        bool SamePlacement(const TerrainPlacement &a, const TerrainPlacement &b)
        {
            return a.asset == b.asset && a.translation.Meters() == b.translation.Meters();
        }

        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            const int64_t q = value / divisor;
            return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? q - 1 : q;
        }

        bool Intersects(const TerrainLocalBounds &a, const TerrainLocalBounds &b)
        {
            return a.minX <= b.maxX && b.minX <= a.maxX && a.minZ <= b.maxZ && b.minZ <= a.maxZ;
        }

        double Distance(const TerrainLocalBounds &rect, double x, double z)
        {
            const double dx = std::max({rect.minX - x, 0.0, x - rect.maxX});
            const double dz = std::max({rect.minZ - z, 0.0, z - rect.maxZ});
            return std::hypot(dx, dz);
        }

        // Where a piece sits in its release.
        struct PieceLayout
        {
            TerrainLocalBounds rect;
            bool seams[4] = {false, false, false, false};
            std::vector<TileCoord> tiles;
            float maxHeight = -std::numeric_limits<float>::infinity();
            double finestSource = kInfinity;
        };

        // Jolt's blocks start every other sample from a shape's first one; pieces that start on an even
        // sample share their blocks with their neighbours where they overlap.
        void AlignToBlocks(TerrainUniformSamples &samples)
        {
            TerrainAreaSamples &area = samples.area;
            const uint32_t padX = (area.samples.min.x & 1) != 0, padZ = (area.samples.min.z & 1) != 0;
            if (!padX && !padZ)
                return;
            const uint32_t countX = area.countX + padX, countZ = area.countZ + padZ;
            std::vector<float> heights(size_t(countX) * countZ, kTerrainColliderHole);
            for (uint32_t z = 0; z < area.countZ; ++z)
                for (uint32_t x = 0; x < area.countX; ++x)
                    heights[size_t(z + padZ) * countX + x + padX] = area.heights[size_t(z) * area.countX + x];
            area.heights = std::move(heights);
            area.countX = countX;
            area.countZ = countZ;
            area.samples.min.x -= padX;
            area.samples.min.z -= padZ;
            const glm::dvec3 origin = area.origin.Meters();
            if (const auto moved = TerrainLocalPosition::FromMeters(
                    {origin.x - padX * area.spacing, origin.y, origin.z - padZ * area.spacing}))
                area.origin = *moved;
        }

        // Sooner first: higher demand, then lower rank, then older.
        struct Order
        {
            int demand = 0;
            float rank = 0.0f;
            uint64_t ticket = 0;

            bool operator<(const Order &other) const
            {
                if (demand != other.demand)
                    return demand > other.demand;
                if (rank != other.rank)
                    return rank < other.rank;
                return ticket < other.ticket;
            }
        };

        struct BuildJob
        {
            uint64_t ticket = 0;
            std::shared_ptr<const TerrainReleaseHandle> release;
            std::vector<std::tuple<uint32_t, TileCoord, std::shared_ptr<const TileHeights>>> sources;
            TerrainLocalBounds rect;
            bool align = false;
            std::vector<double> spacings;
            // Infinite takes the first spacing that builds.
            double maxError = kInfinity;
            float quantizationMin = 0.0f;
            float quantizationMax = 0.0f;
            std::vector<TerrainPlacement> placements;
            std::shared_ptr<const TerrainColliderPhysics> physics;
        };

        struct Built
        {
            uint64_t ticket = 0;
            std::optional<TerrainBuiltCollider> collider;
            // Without its heights.
            TerrainUniformSamples samples;
            double spacing = 0.0;
            TerrainCollisionError error;
            std::string message;
            double seconds = 0.0;
        };

        void Worst(TerrainCollisionError &into, const TerrainCollisionError &error)
        {
            into.geometry = std::max(into.geometry, error.geometry);
            into.quantization = std::max(into.quantization, error.quantization);
            into.horizontal = std::max(into.horizontal, error.horizontal);
            into.numeric = std::max(into.numeric, error.numeric);
            into.total = std::max(into.total, error.total);
        }

        // Worker side: only the job's own heights, the immutable index and the physics side's Build.
        Built Build(const BuildJob &job)
        {
            const auto start = SteadyClock::now();
            Built out;
            out.ticket = job.ticket;
            const TerrainIndex &index = job.release->Index();
            const TerrainSourceTileLookup lookup = [&job](uint32_t layer, TileCoord tile) -> std::shared_ptr<const TileHeights>
            {
                for (const auto &[l, t, heights] : job.sources)
                    if (l == layer && t == tile)
                        return heights;
                return nullptr;
            };
            std::vector<TerrainPlacement> placements = job.placements;
            if (placements.empty())
                placements.push_back({index.release.asset, WorldPosition{}});
            std::string refusals;
            for (const double spacing : job.spacings)
            {
                TerrainResult<TerrainUniformSamples> samples =
                    BuildTerrainUniformSamples(index, lookup, spacing, job.rect);
                if (!samples)
                {
                    refusals += std::format(" [{} m: {}]", spacing, samples.error.message);
                    continue;
                }
                if (job.align)
                    AlignToBlocks(*samples.value);
                std::optional<TerrainBuiltCollider> collider =
                    job.physics->Build(samples.value->area, job.quantizationMin, job.quantizationMax);
                if (!collider)
                {
                    refusals += std::format(" [{} m: no height field]", spacing);
                    continue;
                }
                TerrainCollisionError worst;
                bool covered = true, bounded = true;
                for (const TerrainPlacement &placement : placements)
                {
                    const std::optional<WorldPosition> origin =
                        TryTerrainLocalToWorld(samples.value->area.origin, placement.translation);
                    const std::optional<TerrainCollisionError> error =
                        origin ? TerrainCollisionErrorBudget(*samples.value, placement, *origin,
                                                             collider->quantizationError, collider->horizontalError)
                               : std::nullopt;
                    bounded = bounded && error.has_value();
                    covered = covered && origin &&
                              TerrainCollisionCovers(*samples.value, placement, *origin, collider->extentX,
                                                     collider->extentZ);
                    if (error)
                        Worst(worst, *error);
                }
                if (covered && bounded && worst.total <= job.maxError)
                {
                    samples.value->area.heights = {};
                    out.collider = std::move(collider);
                    out.samples = std::move(*samples.value);
                    out.spacing = spacing;
                    out.error = worst;
                    break;
                }
                if (!covered)
                    refusals += std::format(" [{} m: decoded XY support does not cover the piece]", spacing);
                refusals += bounded ? std::format(" [{} m: total {} = geometry {} + quantization {} + horizontal {} + "
                                                  "numeric {}]",
                                                  spacing, worst.total, worst.geometry, worst.quantization,
                                                  worst.horizontal, worst.numeric)
                                    : std::format(" [{} m: the error proof failed]", spacing);
            }
            if (!out.collider)
                out.message = std::isfinite(job.maxError)
                                  ? std::format("no collision spacing satisfies {} m:{}", job.maxError, refusals)
                                  : std::format("no collision spacing builds:{}", refusals);
            out.seconds = Seconds(SteadyClock::now() - start);
            return out;
        }

        // Shared with every posted job, so a job that runs after the residency is gone finds a live queue.
        class BuildQueue
        {
        public:
            void Push(const Order &order, BuildJob job)
            {
                std::lock_guard lock(mMutex);
                if (mClosed)
                    return;
                mOrders[order.ticket] = order;
                mPending.emplace(order, std::move(job));
            }

            // False once a worker has taken it.
            bool Remove(uint64_t ticket)
            {
                std::lock_guard lock(mMutex);
                const auto it = mOrders.find(ticket);
                if (it == mOrders.end())
                    return false;
                mPending.erase(it->second);
                mOrders.erase(it);
                return true;
            }

            // A posted job builds whatever is soonest when it runs.
            void RunOne()
            {
                BuildJob job;
                {
                    std::lock_guard lock(mMutex);
                    if (mPending.empty())
                        return;
                    const auto first = mPending.begin();
                    mOrders.erase(first->first.ticket);
                    job = std::move(first->second);
                    mPending.erase(first);
                }
                Built built = Build(job);
                std::lock_guard lock(mMutex);
                if (!mClosed)
                    mFinished.push_back(std::move(built));
            }

            std::deque<Built> Take()
            {
                std::lock_guard lock(mMutex);
                return std::exchange(mFinished, {});
            }

            void Clear()
            {
                std::lock_guard lock(mMutex);
                mPending.clear();
                mOrders.clear();
                mFinished.clear();
            }

            void Close()
            {
                std::lock_guard lock(mMutex);
                mClosed = true;
                mPending.clear();
                mOrders.clear();
                mFinished.clear();
            }

        private:
            std::mutex mMutex;
            std::map<Order, BuildJob> mPending;
            std::unordered_map<uint64_t, Order> mOrders;
            std::deque<Built> mFinished;
            bool mClosed = false;
        };
    }

    std::string_view TerrainCollisionStateName(TerrainCollisionState state)
    {
        switch (state)
        {
        case TerrainCollisionState::Ready: return "ready";
        case TerrainCollisionState::CoarseSufficient: return "coarse but sufficient";
        case TerrainCollisionState::Pending: return "pending";
        case TerrainCollisionState::Failed: return "failed";
        }
        return "ready";
    }

    std::string DescribeTerrainCollisionPiece(const TerrainCollisionPieceName &piece)
    {
        if (piece.whole)
            return std::format("whole-map collision of release {}", Hex(piece.release.value));
        return std::format("{}collision piece ({}, {}) of release {}", piece.coarse ? "coarse " : "", piece.x, piece.z,
                           Hex(piece.release.value));
    }

    std::vector<size_t> TerrainCollisionOwners(std::span<const TerrainCollisionPieceArea> pieces,
                                               const TerrainLocalBounds &footprint, bool &unowned)
    {
        std::vector<size_t> candidates, owners;
        for (size_t i = 0; i < pieces.size(); ++i)
        {
            const TerrainCollisionPieceArea &piece = pieces[i];
            const TerrainLocalBounds reach{piece.seams[0] ? piece.rect.minX : -kInfinity,
                                           piece.seams[2] ? piece.rect.minZ : -kInfinity,
                                           piece.seams[1] ? piece.rect.maxX : kInfinity,
                                           piece.seams[3] ? piece.rect.maxZ : kInfinity};
            if (!Intersects(reach, footprint))
                continue;
            candidates.push_back(i);
            const TerrainLocalBounds inner{reach.minX + (piece.seams[0] ? piece.margin : 0.0),
                                           reach.minZ + (piece.seams[2] ? piece.margin : 0.0),
                                           reach.maxX - (piece.seams[1] ? piece.margin : 0.0),
                                           reach.maxZ - (piece.seams[3] ? piece.margin : 0.0)};
            if (footprint.minX >= inner.minX && footprint.maxX <= inner.maxX && footprint.minZ >= inner.minZ &&
                footprint.maxZ <= inner.maxZ)
                owners.push_back(i);
        }
        unowned = owners.empty() && !candidates.empty();
        return unowned ? candidates : owners;
    }

    struct TerrainCollisionResidency::Impl
    {
        struct Collider
        {
            TerrainPlacement placement;
            TerrainColliderId id = 0;
            TerrainCollisionError error;
            bool withinError = true;
        };

        enum class State : uint8_t
        {
            Waiting,
            Building,
            Ready,
            Failed,
        };

        struct Entry
        {
            State state = State::Waiting;
            // A build failure is not retried; a payload failure clears once the payload loads.
            bool buildFailed = false;
            std::string message;
            uint64_t ticket = 0;
            std::shared_ptr<const TerrainColliderShape> shape;
            TerrainBuiltCollider decoded;
            TerrainUniformSamples samples;
            double spacing = 0.0;
            TerrainCollisionError error;
            uint64_t bytes = 0;
            double buildSeconds = 0.0;
            std::vector<Collider> colliders;
            double firstNeeded = std::numeric_limits<double>::quiet_NaN();
            double lastWanted = -kInfinity;
            bool needed = false;
            bool prefetched = false;
            float rank = 0.0f;
        };

        struct Release
        {
            std::shared_ptr<const TerrainReleaseHandle> handle;
            TerrainResidencyMode mode = TerrainResidencyMode::Interest;
            // Built ahead of a release switch, never placed.
            bool staged = false;
            std::vector<TerrainPlacement> placements;
            float minHeight = 0.0f;
            float maxHeight = 0.0f;
            std::unordered_map<uint64_t, PieceLayout> layouts;
            std::optional<PieceLayout> whole;

            const TerrainReleaseInfo &Info() const { return handle->Info(); }
        };

        // What one source needs of one instance.
        struct Need
        {
            size_t release = 0;
            size_t placement = 0;
            PieceKey fine;
        };

        TerrainStreamingSettings::Collision settings;
        std::shared_ptr<TerrainExecutor> executor;
        std::shared_ptr<TerrainColliderPhysics> physics;
        std::shared_ptr<BuildQueue> queue = std::make_shared<BuildQueue>();
        TerrainCollisionPolicy policy;
        std::unordered_map<PieceKey, Entry, PieceKeyHash> entries;
        std::unordered_map<uint64_t, PieceKey> tickets;
        std::vector<Release> releases;
        std::vector<TerrainCollisionAnswer> answers;
        std::vector<std::pair<TerrainReleaseInfo, TerrainCollisionStatus>> stagedStatus;
        std::vector<TerrainCollisionEvent> events;
        std::deque<double> latencies;
        std::optional<TerrainInterestId> interest;
        uint64_t nextTicket = 1;
        TerrainCollisionStats stats;
        std::shared_ptr<TerrainTelemetry> telemetry;

        Impl(const TerrainStreamingSettings::Collision &s, std::shared_ptr<TerrainExecutor> e,
             std::shared_ptr<TerrainColliderPhysics> p)
            : settings(s), executor(std::move(e)), physics(std::move(p))
        {
            policy.spacings = settings.spacingsMeters;
            policy.maxError = settings.maxErrorMeters;
            stats.budgetBytes = settings.budgetBytes;
        }

        ~Impl()
        {
            queue->Close();
            executor.reset();
        }

        Release *FindRelease(const PieceKey &key)
        {
            for (Release &release : releases)
                if (SameRelease(release.Info(), key))
                    return &release;
            return nullptr;
        }

        double Prediction() const
        {
            double latency = settings.loadLatencySeconds;
            for (const double sample : latencies)
                latency = std::max(latency, sample);
            return latency + settings.latencyMarginSeconds;
        }

        const PieceLayout &LayoutOf(Release &release, int32_t px, int32_t pz, bool whole)
        {
            const uint64_t id = (uint64_t(uint32_t(px)) << 32) | uint32_t(pz);
            if (whole && release.whole)
                return *release.whole;
            if (const auto found = release.layouts.find(id); !whole && found != release.layouts.end())
                return found->second;
            const TerrainIndex &index = release.handle->Index();
            const TerrainLayout &grid = index.layout;
            const TerrainLocalBounds &map = grid.Bounds();
            const TileRange mapTiles = grid.Tiles();
            PieceLayout layout;
            if (whole)
                layout.rect = map;
            else
            {
                const int64_t n = settings.pieceTiles;
                const int64_t tx0 = std::max<int64_t>(px * n, mapTiles.min.x);
                const int64_t tz0 = std::max<int64_t>(pz * n, mapTiles.min.z);
                const int64_t tx1 = std::min<int64_t>(px * n + n - 1, mapTiles.max.x);
                const int64_t tz1 = std::min<int64_t>(pz * n + n - 1, mapTiles.max.z);
                const auto edge = [&grid](int64_t tile)
                { return grid.SamplePosition(grid.TileFirstSample(static_cast<int32_t>(tile))); };
                const TerrainLocalBounds core{std::max(map.minX, edge(tx0)), std::max(map.minZ, edge(tz0)),
                                              std::min(map.maxX, edge(tx1 + 1)), std::min(map.maxZ, edge(tz1 + 1))};
                layout.seams[0] = tx0 > mapTiles.min.x;
                layout.seams[1] = tx1 < mapTiles.max.x;
                layout.seams[2] = tz0 > mapTiles.min.z;
                layout.seams[3] = tz1 < mapTiles.max.z;
                layout.rect = core;
                if (layout.seams[1])
                    layout.rect.maxX = std::min(map.maxX, core.maxX + settings.seamOverlapMeters);
                if (layout.seams[3])
                    layout.rect.maxZ = std::min(map.maxZ, core.maxZ + settings.seamOverlapMeters);
            }
            const std::optional<TerrainLayout> covered =
                TerrainLayout::Create(grid.SampleSpacing(), grid.IntervalsPerTile(), layout.rect);
            if (covered)
            {
                const TileRange tiles = covered->Tiles();
                for (int64_t z = tiles.min.z; z <= tiles.max.z; ++z)
                    for (int64_t x = tiles.min.x; x <= tiles.max.x; ++x)
                    {
                        const TileCoord tile{static_cast<int32_t>(x), static_cast<int32_t>(z)};
                        const TerrainTileRecord *record = index.Find(tile);
                        if (!record)
                            continue;
                        layout.tiles.push_back(tile);
                        layout.maxHeight = std::max(layout.maxHeight, record->maxHeight);
                        layout.finestSource = std::min(layout.finestSource, record->sampleSpacing);
                    }
            }
            if (whole)
                return release.whole.emplace(std::move(layout));
            return release.layouts.emplace(id, std::move(layout)).first->second;
        }

        // Index into the policy's spacings of a piece's first fine candidate.
        size_t FineStart(const PieceLayout &layout) const
        {
            for (size_t i = 0; i < policy.spacings.size(); ++i)
                if (policy.spacings[i] <= layout.finestSource)
                    return i;
            return policy.spacings.empty() ? 0 : policy.spacings.size() - 1;
        }

        bool HasCoarse(const PieceLayout &layout) const
        {
            return !policy.spacings.empty() && FineStart(layout) > 0;
        }

        // The part a source may own: seam sides drawn in by the margin, the map's own sides unbounded.
        TerrainLocalBounds Inner(const PieceLayout &layout) const
        {
            const double margin = policy.spacings.empty() ? 0.0 : kTerrainSeamMarginCells * policy.spacings[FineStart(layout)];
            return {layout.seams[0] ? layout.rect.minX + margin : -kInfinity,
                    layout.seams[2] ? layout.rect.minZ + margin : -kInfinity,
                    layout.seams[1] ? layout.rect.maxX - margin : kInfinity,
                    layout.seams[3] ? layout.rect.maxZ - margin : kInfinity};
        }

        Collider *ColliderFor(Entry &entry, const TerrainPlacement &placement)
        {
            for (Collider &collider : entry.colliders)
                if (SamePlacement(collider.placement, placement))
                    return &collider;
            return nullptr;
        }

        void RemoveColliders(Entry &entry)
        {
            for (const Collider &collider : entry.colliders)
                if (collider.id != 0)
                    physics->Remove(collider.id);
            entry.colliders.clear();
        }

        void Drop(std::unordered_map<PieceKey, Entry, PieceKeyHash>::iterator it)
        {
            if (telemetry)
                telemetry->Cancel("collidable", DescribeTerrainCollisionPiece(NameOf(it->first)));
            Entry &entry = it->second;
            if (entry.state == State::Building)
            {
                queue->Remove(entry.ticket);
                tickets.erase(entry.ticket);
            }
            RemoveColliders(entry);
            entries.erase(it);
        }

        void Retire(std::unordered_map<PieceKey, Entry, PieceKeyHash>::iterator it, double now, bool evicted)
        {
            TerrainCollisionEvent event;
            event.kind = TerrainCollisionEvent::Kind::Retired;
            event.piece = NameOf(it->first);
            event.spacing = it->second.spacing;
            event.bytes = it->second.bytes;
            event.colliders = it->second.colliders.size();
            event.unusedSeconds = now - it->second.lastWanted;
            event.message = evicted ? "evicted for a needed piece" : "unused";
            events.push_back(std::move(event));
            ++(evicted ? stats.evicted : stats.retired);
            Drop(it);
        }

        // A later placement than the build saw gets its own error and coverage check.
        void Place(Entry &entry, const PieceKey &key, const TerrainPlacement &placement)
        {
            Collider collider;
            collider.placement = placement;
            const std::optional<WorldPosition> origin =
                TryTerrainLocalToWorld(entry.samples.area.origin, placement.translation);
            const std::optional<TerrainCollisionError> error =
                origin ? TerrainCollisionErrorBudget(entry.samples, placement, *origin,
                                                     entry.decoded.quantizationError, entry.decoded.horizontalError)
                       : std::nullopt;
            const bool covered = origin && TerrainCollisionCovers(entry.samples, placement, *origin,
                                                                  entry.decoded.extentX, entry.decoded.extentZ);
            if (error)
                collider.error = *error;
            collider.withinError = error && covered && (key.coarse || error->total <= policy.maxError);
            if (collider.withinError)
                collider.id = physics->Place(*entry.shape, *origin, placement);
            entry.colliders.push_back(collider);
        }

        uint64_t HeldBytes() const
        {
            uint64_t held = 0;
            for (const auto &[key, entry] : entries)
                if (entry.state == State::Ready)
                    held += entry.bytes;
            return held;
        }

        bool PayloadsOf(const Release &release, const PieceLayout &layout, const TerrainStreamer &streamer,
                        std::vector<std::tuple<uint32_t, TileCoord, std::shared_ptr<const TileHeights>>> *heights,
                        std::string *failure) const
        {
            bool ready = true;
            const TerrainIndex &index = release.handle->Index();
            for (const TileCoord tile : layout.tiles)
            {
                const TerrainTileRecord *record = index.Find(tile);
                const TerrainPayloadKey key = TerrainPayloadKeyOf(release.Info(), *record);
                const TerrainPayloadStatus status = streamer.Status(key);
                if (status.state == TerrainPayloadState::Failed)
                {
                    if (failure)
                        *failure = std::format("{}: {}", DescribeTerrainPayloadKey(key), status.error.message);
                    return false;
                }
                if (status.state != TerrainPayloadState::Ready)
                {
                    ready = false;
                    continue;
                }
                if (heights)
                    heights->emplace_back(record->layer, tile, streamer.Heights(key));
            }
            return ready;
        }

        void StartBuild(Entry &entry, const PieceKey &key, Release &release, const PieceLayout &layout,
                        std::vector<std::tuple<uint32_t, TileCoord, std::shared_ptr<const TileHeights>>> heights)
        {
            BuildJob job;
            job.ticket = nextTicket++;
            job.release = release.handle;
            job.sources = std::move(heights);
            job.rect = layout.rect;
            job.align = !key.whole;
            if (key.whole)
                job.spacings = policy.spacings;
            else if (key.coarse)
                job.spacings = {policy.spacings.front()};
            else
                job.spacings.assign(policy.spacings.begin() + std::ptrdiff_t(FineStart(layout)), policy.spacings.end());
            job.maxError = key.coarse ? kInfinity : policy.maxError;
            job.quantizationMin = release.minHeight;
            job.quantizationMax = release.maxHeight;
            job.placements = release.placements;
            job.physics = physics;
            entry.state = State::Building;
            entry.ticket = job.ticket;
            tickets[job.ticket] = key;
            const int demand = entry.needed ? (key.coarse ? 3 : 2) : 1;
            queue->Push({demand, entry.rank, job.ticket}, std::move(job));
            executor->Post([q = queue] { q->RunOne(); });
            ++stats.builds;
        }
    };

    TerrainCollisionResidency::TerrainCollisionResidency(const TerrainStreamingSettings::Collision &settings,
                                                         std::shared_ptr<TerrainExecutor> executor,
                                                         std::shared_ptr<TerrainColliderPhysics> physics)
        : mImpl(std::make_unique<Impl>(settings, std::move(executor), std::move(physics)))
    {
    }

    TerrainCollisionResidency::~TerrainCollisionResidency() = default;

    void TerrainCollisionResidency::SetInstances(std::span<const TerrainCollisionInstance> instances,
                                                 std::span<const TerrainCollisionInstance> staged)
    {
        Impl &impl = *mImpl;
        std::vector<Impl::Release> next;
        const auto add = [&](const TerrainCollisionInstance &instance, bool stage)
        {
            if (!instance.release)
                return;
            const TerrainReleaseInfo &info = instance.release->Info();
            auto found = std::find_if(next.begin(), next.end(),
                                      [&](const Impl::Release &r) { return r.Info() == info; });
            if (found == next.end())
            {
                Impl::Release release;
                const auto old = std::find_if(impl.releases.begin(), impl.releases.end(),
                                              [&](const Impl::Release &r) { return r.Info() == info; });
                if (old != impl.releases.end())
                    release.layouts = std::move(old->layouts);
                release.handle = instance.release;
                release.mode = instance.mode;
                release.staged = stage;
                release.minHeight = std::numeric_limits<float>::infinity();
                release.maxHeight = -std::numeric_limits<float>::infinity();
                for (const TerrainTileRecord &record : instance.release->Index().tiles)
                    if (record.kind == TerrainPayloadKind::Height && record.source)
                    {
                        release.minHeight = std::min(release.minHeight, record.minHeight);
                        release.maxHeight = std::max(release.maxHeight, record.maxHeight);
                    }
                next.push_back(std::move(release));
                found = next.end() - 1;
            }
            // An active release ignores staged instances of itself.
            else if (found->staged != stage)
                return;
            found->placements.push_back(instance.placement);
        };
        for (const TerrainCollisionInstance &instance : instances)
            add(instance, false);
        for (const TerrainCollisionInstance &instance : staged)
            add(instance, true);
        impl.releases = std::move(next);

        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            const Impl::Release *release = impl.FindRelease(it->first);
            if (!release)
            {
                impl.Drop(it++);
                continue;
            }
            std::erase_if(it->second.colliders,
                          [&](const Impl::Collider &collider)
                          {
                              const bool kept = !release->staged &&
                                                std::any_of(release->placements.begin(), release->placements.end(),
                                                            [&](const TerrainPlacement &p)
                                                            { return SamePlacement(p, collider.placement); });
                              if (!kept && collider.id != 0)
                                  impl.physics->Remove(collider.id);
                              return !kept;
                          });
            ++it;
        }
    }

    void TerrainCollisionResidency::SetPolicy(const TerrainCollisionPolicy &policy)
    {
        if (ValidTerrainCollisionPolicy(policy))
            mImpl->policy = policy;
    }

    void TerrainCollisionResidency::SetTelemetry(std::shared_ptr<TerrainTelemetry> telemetry)
    {
        mImpl->telemetry = std::move(telemetry);
    }

    void TerrainCollisionResidency::Install(double now)
    {
        Impl &impl = *mImpl;
        for (Built &built : impl.queue->Take())
        {
            const auto ticket = impl.tickets.find(built.ticket);
            if (ticket == impl.tickets.end())
            {
                ++impl.stats.discarded;
                continue;
            }
            const PieceKey key = ticket->second;
            impl.tickets.erase(ticket);
            Impl::Entry &entry = impl.entries.at(key);
            impl.stats.buildSeconds += built.seconds;
            impl.stats.maxBuildSeconds = std::max(impl.stats.maxBuildSeconds, built.seconds);
            entry.buildSeconds = built.seconds;
            TerrainCollisionEvent event;
            event.piece = NameOf(key);
            event.buildSeconds = built.seconds;
            if (!built.collider)
            {
                entry.state = Impl::State::Failed;
                entry.buildFailed = true;
                entry.message = std::move(built.message);
                event.kind = TerrainCollisionEvent::Kind::Failed;
                if (impl.telemetry)
                    impl.telemetry->Cancel("collidable", DescribeTerrainCollisionPiece(NameOf(key)));
                event.message = entry.message;
                impl.events.push_back(std::move(event));
                continue;
            }
            entry.state = Impl::State::Ready;
            entry.shape = built.collider->shape;
            entry.decoded = *built.collider;
            entry.decoded.shape.reset();
            entry.bytes = built.collider->bytes;
            entry.samples = std::move(built.samples);
            entry.spacing = built.spacing;
            entry.error = built.error;
            if (std::isfinite(entry.firstNeeded))
            {
                event.latencySeconds = now - entry.firstNeeded;
                // A whole piece is needed from the start, not by a prediction, so its wait does not size one.
                if (!key.whole)
                {
                    impl.latencies.push_back(event.latencySeconds);
                    while (impl.latencies.size() > kLatencySamples)
                        impl.latencies.pop_front();
                }
                impl.stats.maxLatencySeconds = std::max(impl.stats.maxLatencySeconds, event.latencySeconds);
            }
            event.kind = TerrainCollisionEvent::Kind::Placed;
            event.spacing = entry.spacing;
            event.error = entry.error;
            event.bytes = entry.bytes;
            if (Impl::Release *release = impl.FindRelease(key))
            {
                event.staged = release->staged;
                if (!release->staged)
                    for (const TerrainPlacement &placement : release->placements)
                        impl.Place(entry, key, placement);
            }
            for (const Impl::Collider &collider : entry.colliders)
            {
                event.colliders += collider.id != 0;
                Worst(event.error, collider.error);
            }
            if (impl.telemetry && !event.staged && !entry.colliders.empty() &&
                std::all_of(entry.colliders.begin(), entry.colliders.end(), [](const auto &collider) { return collider.id != 0; }))
                impl.telemetry->Complete("collidable", DescribeTerrainCollisionPiece(NameOf(key)));
            impl.events.push_back(std::move(event));
        }

        for (auto &[key, entry] : impl.entries)
        {
            if (entry.state != Impl::State::Ready)
                continue;
            Impl::Release *release = impl.FindRelease(key);
            if (!release || release->staged)
                continue;
            for (const TerrainPlacement &placement : release->placements)
                if (!impl.ColliderFor(entry, placement))
                    impl.Place(entry, key, placement);
        }

        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            Impl::Entry &entry = it->second;
            const bool wanted = entry.needed || entry.prefetched;
            if (entry.state == Impl::State::Ready && !wanted && now - entry.lastWanted >= impl.settings.retainSeconds)
                impl.Retire(it++, now, false);
            else if (entry.state == Impl::State::Failed && !wanted)
                impl.Drop(it++);
            else
                ++it;
        }

        uint64_t held = impl.HeldBytes();
        if (held > impl.settings.budgetBytes)
        {
            std::vector<std::unordered_map<PieceKey, Impl::Entry, PieceKeyHash>::iterator> victims;
            for (auto it = impl.entries.begin(); it != impl.entries.end(); ++it)
                if (it->second.state == Impl::State::Ready && !it->second.needed)
                    victims.push_back(it);
            std::sort(victims.begin(), victims.end(),
                      [](const auto &a, const auto &b)
                      {
                          const Impl::Entry &x = a->second, &y = b->second;
                          if (x.prefetched != y.prefetched)
                              return !x.prefetched;
                          if (!x.prefetched)
                              return x.lastWanted < y.lastWanted;
                          return x.rank > y.rank;
                      });
            for (const auto &victim : victims)
            {
                if (held <= impl.settings.budgetBytes)
                    break;
                held -= victim->second.bytes;
                impl.Retire(victim, now, true);
            }
        }
        impl.stats.overBudgetBytes = held > impl.settings.budgetBytes ? held - impl.settings.budgetBytes : 0;
        impl.stats.peakBytes = std::max(impl.stats.peakBytes, held);
    }

    void TerrainCollisionResidency::Update(std::span<const TerrainCollisionSource> sources, TerrainStreamer &streamer,
                                           double now, double stepSeconds)
    {
        Impl &impl = *mImpl;
        const auto start = SteadyClock::now();
        if (!impl.interest)
            impl.interest = streamer.AddInterest();
        for (auto &[key, entry] : impl.entries)
        {
            entry.needed = false;
            entry.prefetched = false;
            entry.rank = std::numeric_limits<float>::infinity();
        }

        const auto want = [&](const PieceKey &key, bool needed, double rank)
        {
            Impl::Entry &entry = impl.entries[key];
            entry.needed = entry.needed || needed;
            entry.prefetched = entry.prefetched || !needed;
            entry.rank = std::min(entry.rank, static_cast<float>(rank));
            entry.lastWanted = now;
            if (impl.telemetry && needed && entry.state != Impl::State::Ready)
                impl.telemetry->Request("collidable", DescribeTerrainCollisionPiece(NameOf(key)),
                                        TerrainReleaseInfo{.asset = key.asset, .release = key.release});
            if (needed && entry.state != Impl::State::Ready && !std::isfinite(entry.firstNeeded))
                entry.firstNeeded = now;
        };

        for (Impl::Release &release : impl.releases)
            if (release.mode == TerrainResidencyMode::Eager)
                want({release.Info().asset, release.Info().release, 0, 0, true, false}, !release.staged, 0.0);

        const double tau = impl.Prediction();
        std::vector<std::vector<Impl::Need>> needs(sources.size());
        std::vector<std::vector<PieceKey>> stagedNeeds(impl.releases.size());
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const TerrainCollisionSource &source = sources[s];
            const glm::dvec3 velocity = source.velocity;
            for (size_t r = 0; r < impl.releases.size(); ++r)
            {
                Impl::Release &release = impl.releases[r];
                const TerrainLayout &grid = release.handle->Index().layout;
                for (size_t p = 0; p < release.placements.size(); ++p)
                {
                    const TerrainPlacement &placement = release.placements[p];
                    const std::optional<TerrainLocalPosition> local =
                        TryWorldToTerrainLocal(source.position, placement.translation);
                    if (!local)
                        continue;
                    const glm::dvec3 at = local->Meters();
                    const glm::dvec3 shift = glm::dvec3(source.position.Meters()) - at;
                    for (const bool prefetch : {false, true})
                    {
                        const double seconds = prefetch ? tau + impl.settings.prefetchSeconds : tau;
                        const double reach = source.radius + 0.5 * source.acceleration * seconds * seconds;
                        TerrainLocalBounds box{std::min(at.x, at.x + velocity.x * seconds) - reach,
                                               std::min(at.z, at.z + velocity.z * seconds) - reach,
                                               std::max(at.x, at.x + velocity.x * seconds) + reach,
                                               std::max(at.z, at.z + velocity.z * seconds) + reach};
                        double lowest = std::min(at.y, at.y + velocity.y * seconds) - reach;
                        if (source.range)
                        {
                            const glm::dvec3 a = glm::dvec3(source.range->first.Meters()) - shift;
                            const glm::dvec3 b = glm::dvec3(source.range->second.Meters()) - shift;
                            box = {std::min({box.minX, a.x, b.x}), std::min({box.minZ, a.z, b.z}),
                                   std::max({box.maxX, a.x, b.x}), std::max({box.maxZ, a.z, b.z})};
                            lowest = std::min({lowest, a.y, b.y});
                        }
                        if (std::isfinite(source.floor))
                            lowest = std::max(lowest, source.floor - double(placement.translation.Meters().y));
                        if (!Intersects(box, grid.Bounds()))
                            continue;
                        const double slack = std::max(impl.policy.maxError, source.tolerance);
                        if (release.mode == TerrainResidencyMode::Eager)
                        {
                            const PieceLayout &layout = impl.LayoutOf(release, 0, 0, true);
                            if (!prefetch && !release.staged && lowest <= double(layout.maxHeight) + slack)
                                needs[s].push_back({r, p, {release.Info().asset, release.Info().release, 0, 0, true, false}});
                            continue;
                        }
                        const std::optional<TerrainLayout> covered = TerrainLayout::Create(
                            grid.SampleSpacing(), grid.IntervalsPerTile(),
                            {std::max(box.minX - impl.settings.seamOverlapMeters, grid.Bounds().minX),
                             std::max(box.minZ - impl.settings.seamOverlapMeters, grid.Bounds().minZ),
                             std::min(box.maxX, grid.Bounds().maxX), std::min(box.maxZ, grid.Bounds().maxZ)});
                        if (!covered)
                            continue;
                        const TileRange tiles = covered->Tiles();
                        const int64_t n = impl.settings.pieceTiles;
                        for (int64_t pz = FloorDiv(tiles.min.z, n); pz <= FloorDiv(tiles.max.z, n); ++pz)
                            for (int64_t px = FloorDiv(tiles.min.x, n); px <= FloorDiv(tiles.max.x, n); ++px)
                            {
                                const PieceLayout &layout =
                                    impl.LayoutOf(release, static_cast<int32_t>(px), static_cast<int32_t>(pz), false);
                                if (layout.tiles.empty() || !Intersects(impl.Inner(layout), box) ||
                                    lowest > double(layout.maxHeight) + slack)
                                    continue;
                                const PieceKey fine{release.Info().asset, release.Info().release,
                                                    static_cast<int32_t>(px), static_cast<int32_t>(pz), false, false};
                                const double rank = Distance(layout.rect, at.x, at.z);
                                if (prefetch)
                                {
                                    const auto found = impl.entries.find(fine);
                                    if (found == impl.entries.end() || !found->second.needed)
                                        want(fine, false, rank);
                                    continue;
                                }
                                if (release.staged)
                                {
                                    want(fine, false, rank);
                                    if (std::find(stagedNeeds[r].begin(), stagedNeeds[r].end(), fine) == stagedNeeds[r].end())
                                        stagedNeeds[r].push_back(fine);
                                    continue;
                                }
                                want(fine, true, rank);
                                needs[s].push_back({r, p, fine});
                            }
                    }
                }
            }
        }

        // A coarse piece stands in only while its fine one is not collidable.
        std::vector<std::pair<PieceKey, float>> standIns;
        for (auto &[key, entry] : impl.entries)
        {
            if (key.coarse || key.whole || !entry.needed || entry.state == Impl::State::Ready)
                continue;
            Impl::Release *release = impl.FindRelease(key);
            if (release && impl.HasCoarse(impl.LayoutOf(*release, key.x, key.z, false)))
            {
                PieceKey coarse = key;
                coarse.coarse = true;
                standIns.emplace_back(coarse, entry.rank);
            }
        }
        for (const auto &[coarse, rank] : standIns)
        {
            Impl::Entry &stand = impl.entries[coarse];
            stand.needed = true;
            stand.rank = std::min(stand.rank, rank);
            stand.lastWanted = now;
        }

        std::unordered_map<TerrainPayloadKey, TerrainPayloadWant> wants;
        for (auto &[key, entry] : impl.entries)
        {
            if (!entry.needed && !entry.prefetched)
                continue;
            Impl::Release *release = impl.FindRelease(key);
            if (!release)
                continue;
            const PieceLayout &layout = impl.LayoutOf(*release, key.x, key.z, key.whole);
            const TerrainIndex &index = release->handle->Index();
            for (const TileCoord tile : layout.tiles)
            {
                const TerrainPayloadKey payload = TerrainPayloadKeyOf(release->Info(), *index.Find(tile));
                TerrainPayloadWant next{payload, entry.needed ? TerrainDemand::Required : TerrainDemand::Prefetch,
                                        entry.rank};
                const auto [at, inserted] = wants.try_emplace(payload, next);
                if (!inserted)
                {
                    if (next.demand > at->second.demand)
                        at->second = next;
                    else if (next.demand == at->second.demand)
                        at->second.rank = std::min(at->second.rank, next.rank);
                }
            }
        }
        std::vector<TerrainPayloadWant> list;
        list.reserve(wants.size());
        for (const auto &[key, want] : wants)
            list.push_back(want);
        streamer.SetInterest(*impl.interest, list);

        std::vector<std::pair<Order, PieceKey>> ready;
        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            Impl::Entry &entry = it->second;
            const bool wanted = entry.needed || entry.prefetched;
            if (!wanted && (entry.state == Impl::State::Waiting || entry.state == Impl::State::Building))
            {
                impl.Drop(it++);
                continue;
            }
            Impl::Release *release = impl.FindRelease(it->first);
            if (!release)
            {
                ++it;
                continue;
            }
            const PieceLayout &layout = impl.LayoutOf(*release, it->first.x, it->first.z, it->first.whole);
            if (entry.state == Impl::State::Failed && !entry.buildFailed)
            {
                std::string failure;
                if (impl.PayloadsOf(*release, layout, streamer, nullptr, &failure) || failure.empty())
                    entry.state = Impl::State::Waiting;
            }
            if (entry.state == Impl::State::Waiting)
            {
                std::string failure;
                if (impl.PayloadsOf(*release, layout, streamer, nullptr, &failure))
                    ready.push_back({{entry.needed ? (it->first.coarse ? 3 : 2) : 1, entry.rank, 0}, it->first});
                else if (!failure.empty())
                {
                    entry.state = Impl::State::Failed;
                    entry.message = std::move(failure);
                }
            }
            ++it;
        }
        std::sort(ready.begin(), ready.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        uint64_t held = impl.HeldBytes();
        for (const auto &[order, key] : ready)
        {
            Impl::Entry &entry = impl.entries.at(key);
            Impl::Release &release = *impl.FindRelease(key);
            const PieceLayout &layout = impl.LayoutOf(release, key.x, key.z, key.whole);
            if (!entry.needed && held >= impl.settings.budgetBytes)
                continue;
            std::vector<std::tuple<uint32_t, TileCoord, std::shared_ptr<const TileHeights>>> heights;
            if (!impl.PayloadsOf(release, layout, streamer, &heights, nullptr))
                continue;
            impl.StartBuild(entry, key, release, layout, std::move(heights));
        }

        impl.stagedStatus.clear();
        for (size_t r = 0; r < impl.releases.size(); ++r)
        {
            const Impl::Release &release = impl.releases[r];
            if (!release.staged)
                continue;
            if (release.mode == TerrainResidencyMode::Eager)
                stagedNeeds[r] = {{release.Info().asset, release.Info().release, 0, 0, true, false}};
            TerrainCollisionStatus status;
            for (const PieceKey &key : stagedNeeds[r])
            {
                ++status.needed;
                const auto found = impl.entries.find(key);
                const Impl::State state = found == impl.entries.end() ? Impl::State::Waiting : found->second.state;
                if (state == Impl::State::Ready)
                {
                    ++status.ready;
                    status.contactError = std::max(status.contactError, found->second.error.total);
                }
                else if (state == Impl::State::Failed)
                {
                    if (status.failed++ == 0)
                        status.reason = std::format("{}: {}", DescribeTerrainCollisionPiece(NameOf(key)),
                                                    found->second.message);
                }
                else
                    ++status.pending;
            }
            status.state = status.failed    ? TerrainCollisionState::Failed
                           : status.pending ? TerrainCollisionState::Pending
                                            : TerrainCollisionState::Ready;
            impl.stagedStatus.emplace_back(release.Info(), std::move(status));
        }

        impl.answers.assign(sources.size(), {});
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const TerrainCollisionSource &source = sources[s];
            TerrainCollisionAnswer &answer = impl.answers[s];
            TerrainCollisionStatus &status = answer.status;
            for (const Impl::Need &need : needs[s])
            {
                Impl::Release &release = impl.releases[need.release];
                const TerrainPlacement &placement = release.placements[need.placement];
                ++status.needed;
                const auto fineEntry = impl.entries.find(need.fine);
                if (fineEntry == impl.entries.end())
                {
                    ++status.pending;
                    continue;
                }
                Impl::Entry &fine = fineEntry->second;
                Impl::Collider *fineCollider = fine.state == Impl::State::Ready ? impl.ColliderFor(fine, placement) : nullptr;
                if (fineCollider && fineCollider->id != 0)
                {
                    ++status.ready;
                    status.contactError = std::max(status.contactError, fineCollider->error.total);
                    continue;
                }
                PieceKey coarseKey = need.fine;
                coarseKey.coarse = true;
                const auto coarse = impl.entries.find(coarseKey);
                Impl::Collider *coarseCollider = coarse != impl.entries.end() && coarse->second.state == Impl::State::Ready
                                                     ? impl.ColliderFor(coarse->second, placement)
                                                     : nullptr;
                if (coarseCollider && coarseCollider->id != 0 && coarseCollider->error.total <= source.tolerance)
                {
                    ++status.coarse;
                    status.contactError = std::max(status.contactError, coarseCollider->error.total);
                    continue;
                }
                const bool failed = fine.state == Impl::State::Failed || (fineCollider && !fineCollider->withinError);
                if (failed)
                {
                    ++status.failed;
                    if (status.reason.empty())
                        status.reason = std::format("{}: {}", DescribeTerrainCollisionPiece(NameOf(need.fine)),
                                                    fine.state == Impl::State::Failed
                                                        ? fine.message
                                                        : "its contact error exceeds the limit at this placement");
                }
                else
                    ++status.pending;
            }
            status.state = status.failed ? TerrainCollisionState::Failed
                           : status.pending ? TerrainCollisionState::Pending
                           : status.coarse  ? TerrainCollisionState::CoarseSufficient
                                            : TerrainCollisionState::Ready;

            if (!(stepSeconds > 0.0))
                continue;
            const double speed = glm::length(source.velocity);
            const double reach = source.radius + (speed + source.acceleration * stepSeconds) * stepSeconds + kContactSlack;
            for (Impl::Release &release : impl.releases)
            {
                if (release.staged)
                    continue;
                const TerrainLayout &grid = release.handle->Index().layout;
                for (const TerrainPlacement &placement : release.placements)
                {
                    const std::optional<TerrainLocalPosition> local =
                        TryWorldToTerrainLocal(source.position, placement.translation);
                    if (!local)
                        continue;
                    const glm::dvec3 at = local->Meters();
                    const TerrainLocalBounds footprint{at.x - reach, at.z - reach, at.x + reach, at.z + reach};
                    if (!Intersects(footprint, grid.Bounds()))
                        continue;
                    std::vector<TerrainCollisionPieceArea> areas;
                    std::vector<TerrainColliderId> ids;
                    const auto add = [&](const PieceKey &fineKey, const PieceLayout &layout)
                    {
                        for (const bool coarse : {false, true})
                        {
                            PieceKey key = fineKey;
                            key.coarse = coarse;
                            const auto found = impl.entries.find(key);
                            if (found == impl.entries.end() || found->second.state != Impl::State::Ready)
                                continue;
                            const Impl::Collider *collider = impl.ColliderFor(found->second, placement);
                            if (!collider || collider->id == 0)
                                continue;
                            TerrainCollisionPieceArea area;
                            area.rect = layout.rect;
                            std::copy(std::begin(layout.seams), std::end(layout.seams), std::begin(area.seams));
                            area.margin = kTerrainSeamMarginCells * found->second.spacing;
                            areas.push_back(area);
                            ids.push_back(collider->id);
                            return;
                        }
                    };
                    if (release.mode == TerrainResidencyMode::Eager)
                        add({release.Info().asset, release.Info().release, 0, 0, true, false},
                            impl.LayoutOf(release, 0, 0, true));
                    else
                    {
                        const TerrainLocalBounds &map = grid.Bounds();
                        const std::optional<TerrainLayout> covered = TerrainLayout::Create(
                            grid.SampleSpacing(), grid.IntervalsPerTile(),
                            {std::clamp(footprint.minX - impl.settings.seamOverlapMeters, map.minX, map.maxX),
                             std::clamp(footprint.minZ - impl.settings.seamOverlapMeters, map.minZ, map.maxZ),
                             std::clamp(footprint.maxX, map.minX, map.maxX) + 0.0,
                             std::clamp(footprint.maxZ, map.minZ, map.maxZ) + 0.0});
                        if (covered)
                        {
                            const TileRange tiles = covered->Tiles();
                            const int64_t n = impl.settings.pieceTiles;
                            for (int64_t pz = FloorDiv(tiles.min.z, n); pz <= FloorDiv(tiles.max.z, n); ++pz)
                                for (int64_t px = FloorDiv(tiles.min.x, n); px <= FloorDiv(tiles.max.x, n); ++px)
                                    add({release.Info().asset, release.Info().release, static_cast<int32_t>(px),
                                         static_cast<int32_t>(pz), false, false},
                                        impl.LayoutOf(release, static_cast<int32_t>(px), static_cast<int32_t>(pz), false));
                        }
                    }
                    bool unowned = false;
                    for (const size_t i : TerrainCollisionOwners(areas, footprint, unowned))
                        answer.contacts.push_back(ids[i]);
                    answer.unowned = answer.unowned || unowned;
                }
            }
        }

        impl.stats.lastUpdateSeconds = Seconds(SteadyClock::now() - start);
        impl.stats.maxUpdateSeconds = std::max(impl.stats.maxUpdateSeconds, impl.stats.lastUpdateSeconds);
    }

    const TerrainCollisionAnswer &TerrainCollisionResidency::Answer(size_t source) const
    {
        static const TerrainCollisionAnswer kNone;
        return source < mImpl->answers.size() ? mImpl->answers[source] : kNone;
    }

    TerrainCollisionStatus TerrainCollisionResidency::StagedStatus(const TerrainReleaseInfo &release) const
    {
        for (const auto &[info, status] : mImpl->stagedStatus)
            if (info == release)
                return status;
        TerrainCollisionStatus none;
        none.state = TerrainCollisionState::Pending;
        return none;
    }

    void TerrainCollisionResidency::ForEachCollider(
        const std::function<void(const TerrainPlacement &, TerrainColliderId)> &visit) const
    {
        for (const auto &[key, entry] : mImpl->entries)
            for (const Impl::Collider &collider : entry.colliders)
                if (collider.id != 0)
                    visit(collider.placement, collider.id);
    }

    void TerrainCollisionResidency::Clear()
    {
        Impl &impl = *mImpl;
        for (auto &[key, entry] : impl.entries)
        {
            if (impl.telemetry)
                impl.telemetry->Cancel("collidable", DescribeTerrainCollisionPiece(NameOf(key)));
            impl.RemoveColliders(entry);
        }
        impl.queue->Clear();
        impl.tickets.clear();
        impl.entries.clear();
        impl.releases.clear();
        impl.answers.clear();
        impl.stagedStatus.clear();
    }

    double TerrainCollisionResidency::PredictionSeconds() const
    {
        return mImpl->Prediction();
    }

    std::vector<TerrainCollisionEvent> TerrainCollisionResidency::TakeEvents()
    {
        return std::exchange(mImpl->events, {});
    }

    TerrainCollisionStats TerrainCollisionResidency::Stats() const
    {
        const Impl &impl = *mImpl;
        TerrainCollisionStats stats = impl.stats;
        stats.heldBytes = impl.HeldBytes();
        stats.latencySeconds = impl.Prediction() - impl.settings.latencyMarginSeconds;
        for (const auto &[key, entry] : impl.entries)
        {
            switch (entry.state)
            {
            case Impl::State::Waiting: ++stats.waiting; break;
            case Impl::State::Building: ++stats.building; break;
            case Impl::State::Ready: ++stats.ready; break;
            case Impl::State::Failed: ++stats.failed; break;
            }
            for (const Impl::Collider &collider : entry.colliders)
                stats.colliders += collider.id != 0;
        }
        return stats;
    }

    TerrainReleaseUsage TerrainCollisionResidency::Usage(const TerrainReleaseInfo &release) const
    {
        TerrainReleaseUsage usage;
        for (const auto &[key, entry] : mImpl->entries)
        {
            if (!SameRelease(release, key))
                continue;
            switch (entry.state)
            {
            case Impl::State::Waiting: ++usage.waiting; break;
            case Impl::State::Building: ++usage.inFlight; break;
            case Impl::State::Ready:
                ++usage.ready;
                usage.bytes += entry.bytes;
                break;
            case Impl::State::Failed:
                if (usage.failed++ == 0)
                    usage.failure = std::format("{}: {}", DescribeTerrainCollisionPiece(NameOf(key)), entry.message);
                break;
            }
        }
        return usage;
    }

    TerrainCollisionSources::Id TerrainCollisionSources::Add(const TerrainCollisionSource &source)
    {
        const Id id = mNext++;
        mSources.emplace(id, source);
        return id;
    }

    bool TerrainCollisionSources::Set(Id id, const TerrainCollisionSource &source)
    {
        const auto found = mSources.find(id);
        if (found == mSources.end())
            return false;
        found->second = source;
        return true;
    }

    void TerrainCollisionSources::Remove(Id id)
    {
        mSources.erase(id);
        mStatus.erase(id);
    }

    TerrainCollisionStatus TerrainCollisionSources::Status(Id id) const
    {
        const auto found = mStatus.find(id);
        return found == mStatus.end() ? TerrainCollisionStatus{} : found->second;
    }

    void TerrainCollisionSources::SetStatus(Id id, TerrainCollisionStatus status)
    {
        if (mSources.contains(id))
            mStatus[id] = std::move(status);
    }
}
