/**
 * @file TerrainExecutor.h
 * @author Jackey Cen
 * @brief Where terrain background work runs: a small worker pool, or a manual queue for tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace mir
{
    class TerrainExecutor
    {
    public:
        virtual ~TerrainExecutor() = default;

        // Runs the job later on some thread. Jobs never throw.
        virtual void Post(std::function<void()> job) = 0;
    };

    // Jobs not started when the pool is destroyed are dropped unrun.
    class TerrainWorkerPool final : public TerrainExecutor
    {
    public:
        explicit TerrainWorkerPool(uint32_t threads);
        ~TerrainWorkerPool() override;

        TerrainWorkerPool(const TerrainWorkerPool &) = delete;
        TerrainWorkerPool &operator=(const TerrainWorkerPool &) = delete;

        void Post(std::function<void()> job) override;
        size_t ThreadCount() const { return mThreads.size(); }

    private:
        void Work(std::stop_token stop);

        std::mutex mMutex;
        std::condition_variable_any mWake;
        std::deque<std::function<void()>> mJobs;
        // Last, so the threads join before the queue goes.
        std::vector<std::jthread> mThreads;
    };

    // Holds jobs until a test runs them on its own thread.
    class TerrainManualExecutor final : public TerrainExecutor
    {
    public:
        void Post(std::function<void()> job) override;

        size_t Pending() const;
        // Oldest first; false when nothing was queued.
        bool RunOne();
        // Includes jobs the run jobs post.
        size_t RunAll();

    private:
        mutable std::mutex mMutex;
        std::deque<std::function<void()>> mJobs;
    };
}
