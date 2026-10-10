/**
 * @file TerrainStreamer.cpp
 * @author Jackey Cen
 * @brief Schedules asynchronous payload range reads and keeps decoded payloads within a CPU budget.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainStreamer.h"

#include "PackInternal.h"

#include <core/log/Assert.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainScene.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Error;
        using detail::Hex;
        using SteadyClock = std::chrono::steady_clock;
        using Payload = std::variant<std::monostate, std::shared_ptr<const TileHeights>,
                                     std::shared_ptr<const TerrainLodSurface>, std::shared_ptr<const TerrainLodNormals>,
                                     std::shared_ptr<const TerrainRegionMesh>>;

        double Seconds(SteadyClock::duration duration)
        {
            return std::chrono::duration<double>(duration).count();
        }

        // Sooner first: higher demand, lower rank, older want, older read.
        struct Order
        {
            TerrainDemand demand = TerrainDemand::Prefetch;
            float rank = 0.0f;
            uint64_t sequence = 0;
            uint64_t ticket = 0;

            bool operator<(const Order &other) const
            {
                if (demand != other.demand)
                    return demand > other.demand;
                if (rank != other.rank)
                    return rank < other.rank;
                return std::tie(sequence, ticket) < std::tie(other.sequence, other.ticket);
            }
        };

        struct Job
        {
            uint64_t ticket = 0;
            std::shared_ptr<const TerrainReleaseHandle> handle;
            const TerrainTileRecord *record = nullptr;
        };

        struct Finished
        {
            uint64_t ticket = 0;
            Payload payload;
            TerrainPackError error;
            uint64_t bytesRead = 0;
            double readSeconds = 0.0;
            double decodeSeconds = 0.0;
        };

        template <typename T>
        void Keep(TerrainResult<T> decoded, Finished &out)
        {
            if (decoded)
                out.payload = std::make_shared<const T>(std::move(*decoded.value));
            else
                out.error = std::move(decoded.error);
        }

        // Worker side: one range read, its record checksum, then the payload's own checks.
        Finished Process(const Job &job)
        {
            Finished out;
            out.ticket = job.ticket;
            const auto start = SteadyClock::now();
            TerrainResult<std::vector<std::byte>> bytes = job.handle->ReadPayload(*job.record);
            const auto read = SteadyClock::now();
            out.readSeconds = Seconds(read - start);
            if (!bytes)
            {
                out.error = std::move(bytes.error);
                return out;
            }
            out.bytesRead = bytes.value->size();
            const TerrainIndex &index = job.handle->Index();
            const TerrainTileRecord &record = *job.record;
            switch (record.kind)
            {
            case TerrainPayloadKind::Height: Keep(DecodeTerrainTile(index, record, *bytes.value), out); break;
            case TerrainPayloadKind::LodHeight: Keep(DecodeTerrainLod(index, record, *bytes.value), out); break;
            case TerrainPayloadKind::Normal: Keep(DecodeTerrainNormals(index, record, *bytes.value), out); break;
            case TerrainPayloadKind::Region: Keep(DecodeTerrainRegion(index, record, *bytes.value), out); break;
            }
            out.decodeSeconds = Seconds(SteadyClock::now() - read);
            return out;
        }

        // Shared with every posted job, so a job that runs after the streamer is gone finds a live queue.
        class WorkQueue
        {
        public:
            void Push(const Order &order, Job job)
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

            void Reorder(const Order &order)
            {
                std::lock_guard lock(mMutex);
                const auto it = mOrders.find(order.ticket);
                if (it == mOrders.end())
                    return;
                auto node = mPending.extract(it->second);
                node.key() = order;
                mPending.insert(std::move(node));
                it->second = order;
            }

            // A posted job reads whatever is soonest when it runs, not what was pushed with it.
            void RunOne()
            {
                Job job;
                {
                    std::lock_guard lock(mMutex);
                    if (mPending.empty())
                        return;
                    const auto first = mPending.begin();
                    mOrders.erase(first->first.ticket);
                    job = std::move(first->second);
                    mPending.erase(first);
                }
                Finished finished = Process(job);
                {
                    std::lock_guard lock(mMutex);
                    if (mClosed)
                        return;
                    mFinished.push_back(std::move(finished));
                }
                mReady.notify_all();
            }

            std::vector<Finished> Take(size_t max)
            {
                std::lock_guard lock(mMutex);
                std::vector<Finished> taken;
                while (!mFinished.empty() && taken.size() < max)
                {
                    taken.push_back(std::move(mFinished.front()));
                    mFinished.pop_front();
                }
                return taken;
            }

            bool HasFinished()
            {
                std::lock_guard lock(mMutex);
                return !mFinished.empty();
            }

            // Without a timeout, waits until a read finishes.
            bool Wait(std::optional<std::chrono::duration<double>> timeout)
            {
                std::unique_lock lock(mMutex);
                const auto ready = [this] { return !mFinished.empty(); };
                if (timeout)
                    return mReady.wait_for(lock, *timeout, ready);
                mReady.wait(lock, ready);
                return true;
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
            std::condition_variable mReady;
            std::map<Order, Job> mPending;
            std::unordered_map<uint64_t, Order> mOrders;
            std::deque<Finished> mFinished;
            bool mClosed = false;
        };

        template <typename T>
        std::shared_ptr<const T> As(const Payload &payload)
        {
            const auto *held = std::get_if<std::shared_ptr<const T>>(&payload);
            return held ? *held : nullptr;
        }
    }

    TerrainPayloadKey TerrainPayloadKeyOf(const TerrainReleaseInfo &release, const TerrainTileRecord &record)
    {
        return {release.asset, release.release, record.kind, record.level, record.layer, record.tile};
    }

    std::string DescribeTerrainPayloadKey(const TerrainPayloadKey &key)
    {
        const std::string release = Hex(key.release.value);
        switch (key.kind)
        {
        case TerrainPayloadKind::Height:
            return std::format("Height tile ({}, {}) layer {} of release {}", key.coord.x, key.coord.z, key.layer,
                               release);
        case TerrainPayloadKind::LodHeight:
        case TerrainPayloadKind::Normal:
            return std::format("{} tile ({}, {}) layer {} level {} of release {}",
                               key.kind == TerrainPayloadKind::Normal ? "Normal" : "LodHeight", key.coord.x,
                               key.coord.z, key.layer, key.level, release);
        case TerrainPayloadKind::Region:
            return std::format("Region L{} ({}, {}) of release {}", key.level, key.coord.x, key.coord.z, release);
        }
        return std::format("payload kind {} of release {}", static_cast<uint32_t>(key.kind), release);
    }

    uint64_t TerrainPayloadFootprint(const TerrainTileRecord &record)
    {
        const uint64_t vertices = record.vertexCount;
        const uint64_t indices = uint64_t(record.indexCount) * sizeof(uint32_t);
        switch (record.kind)
        {
        case TerrainPayloadKind::Height:
        {
            const uint64_t side = uint64_t(std::max(record.samplesPerSide, 0));
            return sizeof(TileHeights) + side * side * sizeof(float);
        }
        case TerrainPayloadKind::LodHeight: return sizeof(TerrainLodSurface) + vertices * sizeof(glm::vec3) + indices;
        case TerrainPayloadKind::Normal: return sizeof(TerrainLodNormals) + vertices * sizeof(glm::vec3);
        case TerrainPayloadKind::Region: return sizeof(TerrainRegionMesh) + 2 * vertices * sizeof(glm::vec3) + indices;
        }
        return record.length;
    }

    struct TerrainStreamer::Impl
    {
        struct ReleaseState
        {
            TerrainReleaseInfo info;
            // Every live TerrainResidentRelease of this release; their queries all get its heights.
            std::vector<std::weak_ptr<TerrainResidentRelease>> owners;
            std::shared_ptr<const TerrainReleaseHandle> handle;
            TerrainResidencyMode mode = TerrainResidencyMode::Interest;
            uint64_t footprint = 0;
            // Height-query tiles and when they were last asked for or answered from.
            std::unordered_map<TerrainPayloadKey, double> queryUses;

            bool Alive()
            {
                std::erase_if(owners, [](const std::weak_ptr<TerrainResidentRelease> &owner) { return owner.expired(); });
                return !owners.empty();
            }

            template <typename F>
            void ForEachQuery(F &&f)
            {
                for (const std::weak_ptr<TerrainResidentRelease> &weak : owners)
                    if (const std::shared_ptr<TerrainResidentRelease> owner = weak.lock())
                        f(owner->Query());
            }
        };

        struct Entry
        {
            TerrainPayloadKey key;
            std::shared_ptr<ReleaseState> release;
            // Null when the index has no such payload.
            const TerrainTileRecord *record = nullptr;
            TerrainPayloadState state = TerrainPayloadState::Waiting;
            // Merged over every source in this Update; empty when nobody wants it.
            std::optional<TerrainDemand> demand;
            float rank = 0.0f;
            TerrainDemand lastDemand = TerrainDemand::Prefetch;
            double lastWanted = 0.0;
            uint64_t sequence = 0;
            // Meaningful while Loading.
            Order queued;
            uint32_t attempts = 0;
            // End of a retry delay or of a failed hold.
            double notBefore = 0.0;
            uint64_t bytes = 0;
            Payload payload;
            // The last failure, if any.
            TerrainPackError error;
            // Required for as long as its Eager release lives.
            bool eager = false;
        };

        TerrainStreamingSettings settings;
        std::shared_ptr<TerrainExecutor> executor;
        Clock clock;
        std::shared_ptr<WorkQueue> queue = std::make_shared<WorkQueue>();
        std::vector<std::shared_ptr<ReleaseState>> releases;
        std::unordered_map<TerrainPayloadKey, Entry> entries;
        std::unordered_map<uint64_t, TerrainPayloadKey> loadingTickets;
        std::unordered_map<TerrainInterestId, std::vector<TerrainPayloadWant>> interests;
        TerrainInterestId nextInterest = 1;
        uint64_t nextTicket = 0;
        uint64_t nextSequence = 0;
        uint64_t resident = 0;
        uint64_t loading = 0;
        TerrainStreamingStats stats;

        ~Impl()
        {
            queue->Close();
            executor.reset();
        }

        uint64_t Used() const { return resident + loading; }

        std::shared_ptr<ReleaseState> FindRelease(AssetId asset, TerrainReleaseId release) const
        {
            for (const std::shared_ptr<ReleaseState> &state : releases)
                if (state->info.asset == asset && state->info.release == release)
                    return state;
            return nullptr;
        }

        double RetryDelay(uint32_t failures) const
        {
            double delay = settings.retry.firstDelaySeconds;
            for (uint32_t i = 1; i < failures && delay < settings.retry.maxDelaySeconds; ++i)
                delay *= 2.0;
            return std::min(delay, settings.retry.maxDelaySeconds);
        }

        void ForgetInQueries(Entry &e)
        {
            if (e.key.kind == TerrainPayloadKind::Height)
                e.release->ForEachQuery([&](TerrainHeightQuery &query) { query.Evict(e.key.layer, e.key.coord); });
        }

        // Accounting for an entry about to be erased with its release.
        void Drop(Entry &e)
        {
            if (e.state == TerrainPayloadState::Loading)
            {
                queue->Remove(e.queued.ticket);
                loadingTickets.erase(e.queued.ticket);
                loading -= e.bytes;
            }
            else if (e.state == TerrainPayloadState::Ready)
                resident -= e.bytes;
        }

        void Prune()
        {
            for (size_t i = 0; i < releases.size();)
            {
                ReleaseState &release = *releases[i];
                if (release.Alive())
                {
                    ++i;
                    continue;
                }
                for (auto it = entries.begin(); it != entries.end();)
                {
                    if (it->second.release.get() != &release)
                    {
                        ++it;
                        continue;
                    }
                    Drop(it->second);
                    it = entries.erase(it);
                }
                releases.erase(releases.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }

        void Use(ReleaseState &release, TerrainHeightQuery &query, const TerrainPayloadRequest &request, double now,
                 bool asked)
        {
            if (request.release != release.info.release || request.tile.asset != release.info.asset)
                return;
            const TerrainPayloadKey key{release.info.asset, release.info.release, TerrainPayloadKind::Height, 0,
                                        request.layer, request.tile.coord};
            release.queryUses[key] = now;
            if (!asked)
                return;
            const auto it = entries.find(key);
            if (it == entries.end())
                return;
            // A query that lost its copy (replaced, or added after the read) gets the shared one back.
            const Entry &e = it->second;
            if (e.state == TerrainPayloadState::Ready)
                (void)query.Insert(release.info, key.layer, As<TileHeights>(e.payload));
            else if (e.state == TerrainPayloadState::Failed)
                (void)query.MarkFailed(release.info, key.layer, key.coord, e.error);
        }

        void TakeQueryWants(double now)
        {
            for (const std::shared_ptr<ReleaseState> &release : releases)
                for (const std::weak_ptr<TerrainResidentRelease> &weak : release->owners)
                    if (const std::shared_ptr<TerrainResidentRelease> owner = weak.lock())
                    {
                        TerrainHeightQuery &query = owner->Query();
                        for (const TerrainPayloadRequest &request : query.TakePayloadRequests())
                            Use(*release, query, request, now, true);
                        for (const TerrainPayloadRequest &used : query.TakeUsedTiles())
                            Use(*release, query, used, now, false);
                    }
        }

        Entry *FindOrCreate(const TerrainPayloadKey &key)
        {
            auto it = entries.find(key);
            if (it != entries.end())
                return &it->second;
            std::shared_ptr<ReleaseState> release = FindRelease(key.asset, key.release);
            if (!release)
                return nullptr;
            Entry e;
            e.key = key;
            e.release = std::move(release);
            e.sequence = nextSequence++;
            e.record = e.release->handle->Index().Find(key.coord, key.layer, key.kind, key.level);
            if (e.record)
                e.bytes = TerrainPayloadFootprint(*e.record);
            else
            {
                e.state = TerrainPayloadState::Failed;
                e.error = Error(Code::NotFound, "{} is not in the release index", DescribeTerrainPayloadKey(key));
            }
            return &entries.emplace(key, std::move(e)).first->second;
        }

        void AddEager(ReleaseState &release)
        {
            const TerrainIndex &index = release.handle->Index();
            for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
                for (const TerrainTileRecord &record : *records)
                    if (Entry *e = FindOrCreate(TerrainPayloadKeyOf(release.info, record)))
                        e->eager = true;
        }

        void Want(const TerrainPayloadKey &key, TerrainDemand demand, float rank)
        {
            Entry *found = FindOrCreate(key);
            if (!found)
                return;
            // A NaN would break the read order.
            if (std::isnan(rank))
                rank = std::numeric_limits<float>::infinity();
            Entry &e = *found;
            if (!e.demand || demand > *e.demand)
            {
                e.demand = demand;
                e.rank = rank;
            }
            else if (demand == *e.demand)
                e.rank = std::min(e.rank, rank);
        }

        void ApplyDemand(double now)
        {
            for (auto &[key, e] : entries)
            {
                e.demand = e.eager ? std::optional(TerrainDemand::Required) : std::nullopt;
                e.rank = 0.0f;
            }
            for (const std::shared_ptr<ReleaseState> &release : releases)
                for (auto it = release->queryUses.begin(); it != release->queryUses.end();)
                {
                    if (now - it->second >= settings.residency.queryLeaseSeconds)
                    {
                        it = release->queryUses.erase(it);
                        continue;
                    }
                    Want(it->first, TerrainDemand::Query, 0.0f);
                    ++it;
                }
            for (const auto &[id, wants] : interests)
                for (const TerrainPayloadWant &want : wants)
                    Want(want.key, want.demand, want.rank);

            for (auto it = entries.begin(); it != entries.end();)
            {
                Entry &e = it->second;
                if (e.demand)
                {
                    e.lastWanted = now;
                    e.lastDemand = *e.demand;
                    if (e.state == TerrainPayloadState::Loading &&
                        (e.queued.demand != *e.demand || e.queued.rank != e.rank))
                    {
                        e.queued.demand = *e.demand;
                        e.queued.rank = e.rank;
                        queue->Reorder(e.queued);
                    }
                    ++it;
                    continue;
                }
                // Unwanted: a payload stays until evicted, a started read finishes, the rest goes now.
                if (e.state == TerrainPayloadState::Ready ||
                    (e.state == TerrainPayloadState::Loading && !queue->Remove(e.queued.ticket)))
                {
                    ++it;
                    continue;
                }
                if (e.state == TerrainPayloadState::Loading)
                {
                    loadingTickets.erase(e.queued.ticket);
                    loading -= e.bytes;
                }
                if (e.state != TerrainPayloadState::Failed)
                    ++stats.canceled;
                ForgetInQueries(e);
                it = entries.erase(it);
            }
        }

        void Install(double now)
        {
            for (Finished &finished : queue->Take(settings.io.maxInstallsPerFrame))
            {
                stats.readSeconds += finished.readSeconds;
                stats.decodeSeconds += finished.decodeSeconds;
                stats.bytesRead += finished.bytesRead;
                const auto ticket = loadingTickets.find(finished.ticket);
                if (ticket == loadingTickets.end())
                {
                    ++stats.discarded;
                    continue;
                }
                Entry &e = entries.at(ticket->second);
                loadingTickets.erase(ticket);
                loading -= e.bytes;

                if (finished.error.code == Code::None)
                {
                    ++stats.readsSucceeded;
                    e.state = TerrainPayloadState::Ready;
                    e.payload = std::move(finished.payload);
                    e.attempts = 0;
                    e.error = {};
                    resident += e.bytes;
                    if (const std::shared_ptr<const TileHeights> heights = As<TileHeights>(e.payload))
                        e.release->ForEachQuery([&](TerrainHeightQuery &query)
                                                { (void)query.Insert(e.release->info, e.key.layer, heights); });
                    continue;
                }

                ++stats.readsFailed;
                ++e.attempts;
                e.error = std::move(finished.error);
                e.error.message = std::format("{}: {}", DescribeTerrainPayloadKey(e.key), e.error.message);
                // The release is immutable, so only an IO failure can go away by itself.
                if (e.error.code == Code::Io && e.attempts < settings.retry.maxAttempts)
                {
                    ++stats.retries;
                    e.state = TerrainPayloadState::Waiting;
                    e.notBefore = now + RetryDelay(e.attempts);
                    continue;
                }
                e.state = TerrainPayloadState::Failed;
                e.notBefore = now + settings.retry.failedHoldSeconds;
                if (e.key.kind == TerrainPayloadKind::Height)
                    e.release->ForEachQuery([&](TerrainHeightQuery &query)
                                            { (void)query.MarkFailed(e.release->info, e.key.layer, e.key.coord, e.error); });
            }
        }

        void Restart(double now)
        {
            for (auto &[key, e] : entries)
                if (e.state == TerrainPayloadState::Failed && e.demand && e.record && now >= e.notBefore)
                {
                    e.state = TerrainPayloadState::Waiting;
                    e.attempts = 0;
                    e.notBefore = now;
                    ForgetInQueries(e);
                }
        }

        // Bytes freed. A wanted payload goes back to Waiting; an unwanted one is forgotten.
        uint64_t Evict(Entry &e)
        {
            if (e.state == TerrainPayloadState::Loading)
            {
                if (!queue->Remove(e.queued.ticket))
                    return 0;
                loadingTickets.erase(e.queued.ticket);
                loading -= e.bytes;
                e.state = TerrainPayloadState::Waiting;
                return e.bytes;
            }
            const uint64_t bytes = e.bytes;
            resident -= bytes;
            ++stats.evicted;
            ForgetInQueries(e);
            if (e.demand)
            {
                e.state = TerrainPayloadState::Waiting;
                e.payload = {};
                e.attempts = 0;
                return bytes;
            }
            const TerrainPayloadKey key = e.key;
            entries.erase(key);
            return bytes;
        }

        // Frees need bytes from what matters less than below: payloads nobody wants (least recently wanted
        // first), then reads not started and payloads wanted less (least wanted, highest rank first). Unless
        // partial, evicts nothing when that cannot free enough.
        void MakeRoom(uint64_t need, TerrainDemand below, bool partial)
        {
            std::vector<Entry *> victims;
            uint64_t freeable = 0;
            for (auto &[key, e] : entries)
            {
                const bool lesser = !e.demand || *e.demand < below;
                const bool movable = e.state == TerrainPayloadState::Ready ||
                                     (e.state == TerrainPayloadState::Loading && e.demand);
                if (lesser && movable)
                {
                    victims.push_back(&e);
                    freeable += e.bytes;
                }
            }
            if (!partial && freeable < need)
                return;
            std::sort(victims.begin(), victims.end(),
                      [](const Entry *a, const Entry *b)
                      {
                          if (a->demand.has_value() != b->demand.has_value())
                              return !a->demand.has_value();
                          if (!a->demand)
                              return std::tie(a->lastDemand, a->lastWanted, a->sequence) <
                                     std::tie(b->lastDemand, b->lastWanted, b->sequence);
                          if (*a->demand != *b->demand)
                              return *a->demand < *b->demand;
                          if (a->rank != b->rank)
                              return a->rank > b->rank;
                          return a->sequence > b->sequence;
                      });
            uint64_t freed = 0;
            for (Entry *victim : victims)
            {
                if (freed >= need)
                    break;
                freed += Evict(*victim);
            }
        }

        void Dispatch(Entry &e)
        {
            const uint64_t ticket = ++nextTicket;
            e.state = TerrainPayloadState::Loading;
            e.queued = Order{*e.demand, e.rank, e.sequence, ticket};
            loading += e.bytes;
            loadingTickets[ticket] = e.key;
            queue->Push(e.queued, Job{ticket, e.release->handle, e.record});
            executor->Post([work = queue] { work->RunOne(); });
            ++stats.readsStarted;
        }

        void Admit(double now)
        {
            const uint64_t budget = settings.cpu.budgetBytes;
            if (Used() > budget)
                MakeRoom(Used() - budget, TerrainDemand::Required, true);

            std::vector<Entry *> candidates;
            for (auto &[key, e] : entries)
                if (e.state == TerrainPayloadState::Waiting && e.demand && e.record && now >= e.notBefore)
                    candidates.push_back(&e);
            std::sort(candidates.begin(), candidates.end(),
                      [](const Entry *a, const Entry *b)
                      { return Order{*a->demand, a->rank, a->sequence, 0} < Order{*b->demand, b->rank, b->sequence, 0}; });
            for (Entry *candidate : candidates)
            {
                const bool required = *candidate->demand == TerrainDemand::Required;
                if (Used() + candidate->bytes > budget)
                    MakeRoom(Used() + candidate->bytes - budget, *candidate->demand, required);
                // Lower demand waits behind it rather than overtaking with smaller payloads.
                if (Used() + candidate->bytes > budget && !required)
                    break;
                Dispatch(*candidate);
            }
        }

        void Update()
        {
            const auto started = SteadyClock::now();
            const double now = clock();
            Prune();
            TakeQueryWants(now);
            ApplyDemand(now);
            Install(now);
            Restart(now);
            Admit(now);
            stats.peakBytes = std::max(stats.peakBytes, Used());
            stats.lastUpdateSeconds = Seconds(SteadyClock::now() - started);
            stats.maxUpdateSeconds = std::max(stats.maxUpdateSeconds, stats.lastUpdateSeconds);
        }
    };

    TerrainStreamer::TerrainStreamer(const TerrainStreamingSettings &settings,
                                     std::shared_ptr<TerrainExecutor> executor, Clock clock)
        : mImpl(std::make_unique<Impl>())
    {
        MIR_ASSERT(ValidateTerrainStreamingSettings(settings).empty(), "terrain streaming settings do not validate");
        MIR_ASSERT(executor != nullptr, "a terrain streamer needs an executor");
        mImpl->settings = settings;
        mImpl->executor = std::move(executor);
        if (!clock)
        {
            const auto origin = SteadyClock::now();
            clock = [origin] { return Seconds(SteadyClock::now() - origin); };
        }
        mImpl->clock = std::move(clock);
    }

    TerrainStreamer::~TerrainStreamer() = default;

    const TerrainStreamingSettings &TerrainStreamer::Settings() const
    {
        return mImpl->settings;
    }

    TerrainResidencyMode TerrainStreamer::Add(const std::shared_ptr<TerrainResidentRelease> &release)
    {
        MIR_ASSERT(release != nullptr, "TerrainStreamer::Add: no release");
        const TerrainReleaseInfo &info = release->Info();
        if (const std::shared_ptr<Impl::ReleaseState> known = mImpl->FindRelease(info.asset, info.release))
        {
            const bool owned = std::any_of(known->owners.begin(), known->owners.end(),
                                           [&](const std::weak_ptr<TerrainResidentRelease> &owner)
                                           { return owner.lock() == release; });
            if (!owned)
                known->owners.push_back(release);
            return known->mode;
        }

        auto state = std::make_shared<Impl::ReleaseState>();
        state->info = info;
        state->owners.push_back(release);
        state->handle = release->SharedHandle();
        for (const TerrainTileRecord &record : state->handle->Index().tiles)
            state->footprint += TerrainPayloadFootprint(record);
        for (const TerrainTileRecord &record : state->handle->Index().regions)
            state->footprint += TerrainPayloadFootprint(record);

        TerrainResidencyMode mode = *mImpl->settings.residency.mode;
        if (mode == TerrainResidencyMode::Auto)
        {
            uint64_t eager = 0;
            for (const std::shared_ptr<Impl::ReleaseState> &other : mImpl->releases)
                if (other->mode == TerrainResidencyMode::Eager && other->Alive())
                    eager += other->footprint;
            const uint64_t budget = mImpl->settings.cpu.budgetBytes;
            mode = eager <= budget && state->footprint <= budget - eager ? TerrainResidencyMode::Eager
                                                                         : TerrainResidencyMode::Interest;
        }
        state->mode = mode;
        mImpl->releases.push_back(state);
        if (mode == TerrainResidencyMode::Eager)
            mImpl->AddEager(*state);
        return mode;
    }

    std::shared_ptr<TerrainResidentRelease> TerrainStreamer::Find(AssetId asset, TerrainReleaseId release) const
    {
        if (const std::shared_ptr<Impl::ReleaseState> state = mImpl->FindRelease(asset, release))
            for (const std::weak_ptr<TerrainResidentRelease> &owner : state->owners)
                if (std::shared_ptr<TerrainResidentRelease> live = owner.lock())
                    return live;
        return nullptr;
    }

    std::optional<TerrainResidencyMode> TerrainStreamer::ModeOf(const TerrainReleaseInfo &release) const
    {
        const std::shared_ptr<Impl::ReleaseState> state = mImpl->FindRelease(release.asset, release.release);
        if (!state || state->info != release)
            return std::nullopt;
        return state->mode;
    }

    TerrainInterestId TerrainStreamer::AddInterest()
    {
        const TerrainInterestId id = mImpl->nextInterest++;
        mImpl->interests[id];
        return id;
    }

    void TerrainStreamer::SetInterest(TerrainInterestId interest, std::span<const TerrainPayloadWant> wants)
    {
        const auto it = mImpl->interests.find(interest);
        if (it != mImpl->interests.end())
            it->second.assign(wants.begin(), wants.end());
    }

    void TerrainStreamer::RemoveInterest(TerrainInterestId interest)
    {
        mImpl->interests.erase(interest);
    }

    void TerrainStreamer::Update()
    {
        mImpl->Update();
    }

    TerrainPackError TerrainStreamer::WaitForEager(const TerrainReleaseInfo &release)
    {
        Impl &impl = *mImpl;
        const std::shared_ptr<Impl::ReleaseState> state = impl.FindRelease(release.asset, release.release);
        if (!state || state->info != release || state->mode != TerrainResidencyMode::Eager)
            return Error(Code::InvalidInput, "release {} is not an Eager release of this streamer",
                         Hex(release.release.value));
        const TerrainIndex &index = state->handle->Index();
        for (;;)
        {
            impl.Update();
            bool ready = true;
            std::optional<double> retryAt;
            for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
                for (const TerrainTileRecord &record : *records)
                {
                    const auto it = impl.entries.find(TerrainPayloadKeyOf(release, record));
                    if (it != impl.entries.end() && it->second.state == TerrainPayloadState::Ready)
                        continue;
                    ready = false;
                    if (it == impl.entries.end())
                        continue;
                    if (it->second.state == TerrainPayloadState::Failed)
                        return it->second.error;
                    if (it->second.state == TerrainPayloadState::Waiting)
                        retryAt = std::min(retryAt.value_or(it->second.notBefore), it->second.notBefore);
                }
            if (ready)
                return {};
            if (impl.queue->HasFinished())
                continue;
            std::optional<std::chrono::duration<double>> timeout;
            if (retryAt)
                timeout = std::chrono::duration<double>(std::max(0.0, *retryAt - impl.clock()));
            if (impl.loadingTickets.empty() && !timeout)
                return Error(Code::Io, "release {}: Eager payloads stopped loading", Hex(release.release.value));
            impl.queue->Wait(timeout);
        }
    }

    bool TerrainStreamer::WaitForCompletion(std::chrono::duration<double> timeout)
    {
        return mImpl->queue->Wait(timeout);
    }

    TerrainPayloadStatus TerrainStreamer::Status(const TerrainPayloadKey &key) const
    {
        const auto it = mImpl->entries.find(key);
        if (it == mImpl->entries.end())
            return {};
        return {it->second.state, it->second.attempts, it->second.error};
    }

    std::shared_ptr<const TileHeights> TerrainStreamer::Heights(const TerrainPayloadKey &key) const
    {
        const auto it = mImpl->entries.find(key);
        return it == mImpl->entries.end() ? nullptr : As<TileHeights>(it->second.payload);
    }

    std::shared_ptr<const TerrainLodSurface> TerrainStreamer::Lod(const TerrainPayloadKey &key) const
    {
        const auto it = mImpl->entries.find(key);
        return it == mImpl->entries.end() ? nullptr : As<TerrainLodSurface>(it->second.payload);
    }

    std::shared_ptr<const TerrainLodNormals> TerrainStreamer::Normals(const TerrainPayloadKey &key) const
    {
        const auto it = mImpl->entries.find(key);
        return it == mImpl->entries.end() ? nullptr : As<TerrainLodNormals>(it->second.payload);
    }

    std::shared_ptr<const TerrainRegionMesh> TerrainStreamer::Region(const TerrainPayloadKey &key) const
    {
        const auto it = mImpl->entries.find(key);
        return it == mImpl->entries.end() ? nullptr : As<TerrainRegionMesh>(it->second.payload);
    }

    void TerrainStreamer::RetryFailed()
    {
        for (auto &[key, e] : mImpl->entries)
            if (e.state == TerrainPayloadState::Failed)
                e.notBefore = -std::numeric_limits<double>::infinity();
    }

    TerrainStreamingStats TerrainStreamer::Stats() const
    {
        const Impl &impl = *mImpl;
        TerrainStreamingStats stats = impl.stats;
        stats.budgetBytes = impl.settings.cpu.budgetBytes;
        stats.residentBytes = impl.resident;
        stats.loadingBytes = impl.loading;
        stats.overBudgetBytes = impl.Used() > stats.budgetBytes ? impl.Used() - stats.budgetBytes : 0;
        for (const auto &[key, e] : impl.entries)
            switch (e.state)
            {
            case TerrainPayloadState::Waiting: ++stats.waiting; break;
            case TerrainPayloadState::Loading: ++stats.loading; break;
            case TerrainPayloadState::Ready: ++stats.ready; break;
            case TerrainPayloadState::Failed: ++stats.failed; break;
            case TerrainPayloadState::Absent: break;
            }
        return stats;
    }
}
