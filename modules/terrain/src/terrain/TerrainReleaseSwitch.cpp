/**
 * @file TerrainReleaseSwitch.cpp
 * @author Jackey Cen
 * @brief Moves a loaded terrain asset to the release current names once that release is verified and warm.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainReleaseSwitch.h"

#include "PackInternal.h"

#include <core/log/Assert.h>
#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainStreamer.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using SteadyClock = std::chrono::steady_clock;
        using detail::Hex;

        double Seconds(SteadyClock::duration duration)
        {
            return std::chrono::duration<double>(duration).count();
        }

        struct Job
        {
            uint64_t ticket = 0;
            AssetId asset;
            TerrainReleaseId release;
            // Null opens the release; otherwise checks one of its packs.
            std::shared_ptr<const TerrainReleaseHandle> handle;
            size_t pack = 0;
        };

        struct Result
        {
            uint64_t ticket = 0;
            TerrainPackError error;
            std::shared_ptr<const TerrainReleaseHandle> handle;
            std::vector<TerrainStoredRelease> stored;
            uint64_t bytes = 0;
            double seconds = 0.0;
        };

        // Shared with every posted job, so a job that runs after the switch is gone finds a live queue.
        class ResultQueue
        {
        public:
            void Push(Result result)
            {
                std::lock_guard lock(mMutex);
                if (!mClosed)
                    mResults.push_back(std::move(result));
            }

            std::deque<Result> Take()
            {
                std::lock_guard lock(mMutex);
                return std::exchange(mResults, {});
            }

            void Close()
            {
                std::lock_guard lock(mMutex);
                mClosed = true;
                mResults.clear();
            }

        private:
            std::mutex mMutex;
            std::deque<Result> mResults;
            bool mClosed = false;
        };

        // Worker side: the opener, the release directory listing and the job's own handle.
        Result Run(const Job &job, const TerrainReleaseOpener &open, const std::filesystem::path &root)
        {
            const auto start = SteadyClock::now();
            Result result;
            result.ticket = job.ticket;
            if (job.handle)
            {
                result.error = job.handle->VerifyPack(job.pack);
                result.bytes = job.handle->Index().packs[job.pack].fileSize;
            }
            else
            {
                TerrainResult<TerrainReleaseHandle> opened = open(job.asset, job.release);
                if (!opened)
                    result.error = std::move(opened.error);
                else if (opened.value->Info().asset != job.asset || opened.value->Info().release != job.release)
                    result.error = detail::Error(Code::WrongIdentity, "its index is asset {} release {}",
                                                 Hex(opened.value->Info().asset.value),
                                                 Hex(opened.value->Info().release.value));
                else
                    result.handle = std::make_shared<const TerrainReleaseHandle>(std::move(*opened.value));
                result.stored = ListTerrainReleases(root, job.asset);
            }
            result.seconds = Seconds(SteadyClock::now() - start);
            return result;
        }
    }

    TerrainPrewarmReport MeasureTerrainPrewarm(const TerrainReleaseInfo &release, const TerrainStreamer &streamer,
                                               const TerrainMeshResidency *meshes,
                                               const TerrainCollisionResidency *collision, bool viewsWarm)
    {
        const TerrainReleaseUsage cpu = streamer.Usage(release);
        const TerrainReleaseUsage gpu = meshes ? meshes->Usage(release) : TerrainReleaseUsage{};
        const TerrainReleaseUsage pieces = collision ? collision->Usage(release) : TerrainReleaseUsage{};
        const TerrainCollisionStatus needed = collision ? collision->StagedStatus(release) : TerrainCollisionStatus{};

        TerrainPrewarmReport report;
        for (const TerrainReleaseUsage *usage : {&cpu, &gpu, &pieces})
            if (usage->failed > 0 && report.failure.empty())
                report.failure = usage->failure;
        if (needed.failed > 0 && report.failure.empty())
            report.failure = needed.reason;
        report.inFlight = cpu.inFlight + gpu.inFlight + pieces.inFlight > 0;
        report.warm = viewsWarm && needed.state == TerrainCollisionState::Ready;
        return report;
    }

    std::string_view TerrainSwitchStateName(TerrainSwitchState state)
    {
        switch (state)
        {
        case TerrainSwitchState::Verifying: return "verifying";
        case TerrainSwitchState::Prewarming: return "prewarming";
        case TerrainSwitchState::Ready: return "ready";
        case TerrainSwitchState::Failed: return "failed";
        }
        return "verifying";
    }

    struct TerrainReleaseSwitch::Impl
    {
        struct Candidate
        {
            TerrainReleaseCandidate info;
            std::shared_ptr<const TerrainReleaseHandle> handle;
            std::shared_ptr<TerrainResidentRelease> staged;
            // Of the job in flight; zero for none.
            uint64_t ticket = 0;
            uint32_t idle = 0;
        };

        std::shared_ptr<TerrainExecutor> executor;
        std::filesystem::path root;
        TerrainReleaseOpener open;
        Clock clock;
        std::shared_ptr<ResultQueue> queue = std::make_shared<ResultQueue>();
        // In the order they started, which is the order they are checked in.
        std::vector<Candidate> candidates;
        // What was last reported for a loaded asset whose current cannot be read.
        std::unordered_map<uint64_t, std::string> unreadable;
        std::vector<TerrainSwitchEvent> events;
        TerrainSwitchStats stats;
        uint64_t nextTicket = 1;
        uint64_t stagingVersion = 0;
        bool suspended = false;

        ~Impl() { queue->Close(); }

        Candidate *Find(AssetId asset)
        {
            for (Candidate &candidate : candidates)
                if (candidate.info.asset == asset)
                    return &candidate;
            return nullptr;
        }

        void Event(TerrainSwitchEvent::Kind kind, const TerrainReleaseCandidate &candidate)
        {
            events.push_back({kind, candidate});
        }

        void LetGo(Candidate &candidate)
        {
            if (!candidate.staged)
                return;
            candidate.staged.reset();
            ++stagingVersion;
        }

        // Its job's result, if any, is discarded on arrival.
        void Drop(AssetId asset, std::string why)
        {
            const auto it = std::find_if(candidates.begin(), candidates.end(),
                                         [&](const Candidate &c) { return c.info.asset == asset; });
            if (it == candidates.end())
                return;
            LetGo(*it);
            if (it->info.state != TerrainSwitchState::Failed)
            {
                it->info.failure = std::move(why);
                Event(TerrainSwitchEvent::Kind::Dropped, it->info);
            }
            candidates.erase(it);
        }

        void Fail(Candidate &candidate, std::string why)
        {
            LetGo(candidate);
            candidate.handle.reset();
            candidate.info.state = TerrainSwitchState::Failed;
            candidate.info.failure = std::move(why);
            ++stats.failed;
            Event(TerrainSwitchEvent::Kind::Failed, candidate.info);
        }

        void Settle(Candidate &candidate, bool warm)
        {
            candidate.info.state = TerrainSwitchState::Ready;
            candidate.info.warm = warm;
            candidate.info.readyAt = clock();
            Event(TerrainSwitchEvent::Kind::Ready, candidate.info);
        }

        void Post()
        {
            if (suspended)
                return;
            for (const Candidate &candidate : candidates)
                if (candidate.ticket != 0)
                    return;
            for (Candidate &candidate : candidates)
            {
                if (candidate.info.state != TerrainSwitchState::Verifying)
                    continue;
                Job job;
                job.ticket = nextTicket++;
                job.asset = candidate.info.asset;
                job.release = candidate.info.release;
                job.handle = candidate.handle;
                job.pack = candidate.info.packsVerified;
                candidate.ticket = job.ticket;
                executor->Post([queue = queue, job = std::move(job), open = open, root = root]
                               { queue->Push(Run(job, open, root)); });
                return;
            }
        }
    };

    TerrainReleaseSwitch::TerrainReleaseSwitch(std::shared_ptr<TerrainExecutor> executor,
                                               std::filesystem::path releasesRoot, TerrainReleaseOpener open,
                                               Clock clock)
        : mImpl(std::make_unique<Impl>())
    {
        MIR_ASSERT(executor != nullptr, "a terrain release switch needs an executor");
        mImpl->executor = std::move(executor);
        mImpl->open = open ? std::move(open) : TerrainReleaseFileOpener(releasesRoot);
        mImpl->root = std::move(releasesRoot);
        if (!clock)
        {
            const auto origin = SteadyClock::now();
            clock = [origin] { return Seconds(SteadyClock::now() - origin); };
        }
        mImpl->clock = std::move(clock);
    }

    TerrainReleaseSwitch::~TerrainReleaseSwitch() = default;

    bool TerrainReleaseSwitch::Poll(const TerrainWorld &world)
    {
        Impl &impl = *mImpl;
        bool resolve = false;
        std::vector<AssetId> present;
        for (const TerrainWorld::AssetState &asset : world.Assets())
        {
            present.push_back(asset.asset);
            const TerrainResult<TerrainReleaseId> current = PeekTerrainCurrentPointer(impl.root, asset.asset);
            const std::optional<TerrainReleaseId> now = current ? current.value : std::nullopt;
            if (!asset.release)
            {
                resolve = resolve || now != asset.seenCurrent;
                continue;
            }

            const TerrainReleaseId loaded = asset.release->Info().release;
            if (!current)
            {
                impl.Drop(asset.asset, "current cannot be read");
                std::string &reported = impl.unreadable[asset.asset.value];
                if (reported != current.error.message)
                {
                    reported = current.error.message;
                    TerrainReleaseCandidate candidate;
                    candidate.asset = asset.asset;
                    candidate.logicalPath = asset.logicalPath;
                    candidate.loaded = loaded;
                    candidate.state = TerrainSwitchState::Failed;
                    candidate.startedAt = impl.clock();
                    candidate.failure = current.error.message;
                    impl.Event(TerrainSwitchEvent::Kind::Unreadable, candidate);
                }
                continue;
            }
            impl.unreadable.erase(asset.asset.value);

            Impl::Candidate *candidate = impl.Find(asset.asset);
            if (*now == loaded)
            {
                impl.Drop(asset.asset, "current names the loaded release again");
                continue;
            }
            if (candidate && candidate->info.release == *now && candidate->info.loaded == loaded)
                continue;
            impl.Drop(asset.asset, std::format("current moved on to release {}", Hex(now->value)));

            Impl::Candidate started;
            started.info.asset = asset.asset;
            started.info.logicalPath = asset.logicalPath;
            started.info.loaded = loaded;
            started.info.release = *now;
            started.info.startedAt = impl.clock();
            impl.Event(TerrainSwitchEvent::Kind::Started, started.info);
            impl.candidates.push_back(std::move(started));
        }

        for (size_t i = impl.candidates.size(); i-- > 0;)
            if (std::find(present.begin(), present.end(), impl.candidates[i].info.asset) == present.end())
                impl.Drop(impl.candidates[i].info.asset, "the asset left the scene");
        std::erase_if(impl.unreadable, [&](const auto &entry)
                      { return std::none_of(present.begin(), present.end(),
                                            [&](AssetId asset) { return asset.value == entry.first; }); });
        impl.Post();
        return resolve;
    }

    void TerrainReleaseSwitch::Update(TerrainStreamer &streamer)
    {
        Impl &impl = *mImpl;
        for (Result &result : impl.queue->Take())
        {
            const auto candidate = std::find_if(impl.candidates.begin(), impl.candidates.end(),
                                                [&](const Impl::Candidate &c) { return c.ticket == result.ticket; });
            if (candidate == impl.candidates.end())
            {
                ++impl.stats.discarded;
                continue;
            }
            candidate->ticket = 0;
            ++impl.stats.jobs;
            TerrainReleaseCandidate &info = candidate->info;
            info.verifySeconds += result.seconds;
            if (result.error.code != Code::None)
            {
                impl.Fail(*candidate, std::move(result.error.message));
                continue;
            }
            if (result.handle)
            {
                candidate->handle = std::move(result.handle);
                info.packs = candidate->handle->Index().packs.size();
                info.stored = std::move(result.stored);
            }
            else
            {
                ++info.packsVerified;
                info.bytesVerified += result.bytes;
                impl.stats.bytesVerified += result.bytes;
            }
            if (candidate->handle && info.packsVerified == info.packs)
            {
                info.state = TerrainSwitchState::Prewarming;
                info.verifiedAt = impl.clock();
                impl.Event(TerrainSwitchEvent::Kind::Verified, info);
            }
        }

        if (!impl.suspended)
            for (Impl::Candidate &candidate : impl.candidates)
            {
                const bool verified = candidate.info.state == TerrainSwitchState::Prewarming ||
                                      candidate.info.state == TerrainSwitchState::Ready;
                if (!verified || candidate.staged)
                    continue;
                candidate.staged = TerrainResidentRelease::Create(candidate.handle);
                candidate.idle = 0;
                streamer.Add(candidate.staged, true);
                ++impl.stagingVersion;
            }
        impl.Post();
    }

    std::vector<std::shared_ptr<TerrainResidentRelease>> TerrainReleaseSwitch::Staged() const
    {
        std::vector<std::shared_ptr<TerrainResidentRelease>> staged;
        for (const Impl::Candidate &candidate : mImpl->candidates)
            if (candidate.staged)
                staged.push_back(candidate.staged);
        return staged;
    }

    uint64_t TerrainReleaseSwitch::StagingVersion() const
    {
        return mImpl->stagingVersion;
    }

    void TerrainReleaseSwitch::Report(AssetId asset, const TerrainPrewarmReport &report)
    {
        Impl &impl = *mImpl;
        Impl::Candidate *candidate = impl.Find(asset);
        if (!candidate || !candidate->staged || candidate->info.state != TerrainSwitchState::Prewarming)
            return;
        if (!report.failure.empty())
            impl.Fail(*candidate, report.failure);
        else if (report.warm)
            impl.Settle(*candidate, true);
        else if (report.inFlight)
            candidate->idle = 0;
        else if (++candidate->idle >= kTerrainSwitchIdleReports)
            impl.Settle(*candidate, false);
    }

    std::vector<TerrainReleaseAdoption> TerrainReleaseSwitch::TakeReady()
    {
        Impl &impl = *mImpl;
        std::vector<TerrainReleaseAdoption> ready;
        if (impl.suspended)
            return ready;
        for (auto it = impl.candidates.begin(); it != impl.candidates.end();)
        {
            if (it->info.state != TerrainSwitchState::Ready || !it->staged)
            {
                ++it;
                continue;
            }
            ready.push_back({it->info, std::move(it->staged)});
            ++impl.stats.switched;
            ++impl.stagingVersion;
            it = impl.candidates.erase(it);
        }
        return ready;
    }

    void TerrainReleaseSwitch::Suspend()
    {
        Impl &impl = *mImpl;
        impl.suspended = true;
        for (Impl::Candidate &candidate : impl.candidates)
        {
            impl.LetGo(candidate);
            candidate.idle = 0;
            if (candidate.info.state == TerrainSwitchState::Ready)
                candidate.info.state = TerrainSwitchState::Prewarming;
        }
    }

    void TerrainReleaseSwitch::Resume()
    {
        mImpl->suspended = false;
        mImpl->Post();
    }

    bool TerrainReleaseSwitch::Suspended() const
    {
        return mImpl->suspended;
    }

    void TerrainReleaseSwitch::Clear()
    {
        Impl &impl = *mImpl;
        for (Impl::Candidate &candidate : impl.candidates)
            impl.LetGo(candidate);
        impl.candidates.clear();
        impl.unreadable.clear();
    }

    std::vector<TerrainReleaseCandidate> TerrainReleaseSwitch::Candidates() const
    {
        std::vector<TerrainReleaseCandidate> candidates;
        for (const Impl::Candidate &candidate : mImpl->candidates)
            candidates.push_back(candidate.info);
        return candidates;
    }

    std::vector<TerrainSwitchEvent> TerrainReleaseSwitch::TakeEvents()
    {
        return std::exchange(mImpl->events, {});
    }

    TerrainSwitchStats TerrainReleaseSwitch::Stats() const
    {
        return mImpl->stats;
    }
}
