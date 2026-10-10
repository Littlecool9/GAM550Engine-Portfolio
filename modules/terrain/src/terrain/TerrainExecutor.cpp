/**
 * @file TerrainExecutor.cpp
 * @author Jackey Cen
 * @brief Where terrain background work runs: a small worker pool, or a manual queue for tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainExecutor.h"

#include <utility>

namespace mir
{
    TerrainWorkerPool::TerrainWorkerPool(uint32_t threads)
    {
        mThreads.reserve(threads);
        for (uint32_t i = 0; i < threads; ++i)
            mThreads.emplace_back([this](std::stop_token stop) { Work(stop); });
    }

    TerrainWorkerPool::~TerrainWorkerPool()
    {
        for (std::jthread &thread : mThreads)
            thread.request_stop();
        mWake.notify_all();
        mThreads.clear();
    }

    void TerrainWorkerPool::Post(std::function<void()> job)
    {
        {
            std::lock_guard lock(mMutex);
            mJobs.push_back(std::move(job));
        }
        mWake.notify_one();
    }

    void TerrainWorkerPool::Work(std::stop_token stop)
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(mMutex);
                if (!mWake.wait(lock, stop, [this] { return !mJobs.empty(); }) || stop.stop_requested())
                    return;
                job = std::move(mJobs.front());
                mJobs.pop_front();
            }
            job();
        }
    }

    void TerrainManualExecutor::Post(std::function<void()> job)
    {
        std::lock_guard lock(mMutex);
        mJobs.push_back(std::move(job));
    }

    size_t TerrainManualExecutor::Pending() const
    {
        std::lock_guard lock(mMutex);
        return mJobs.size();
    }

    bool TerrainManualExecutor::RunOne()
    {
        std::function<void()> job;
        {
            std::lock_guard lock(mMutex);
            if (mJobs.empty())
                return false;
            job = std::move(mJobs.front());
            mJobs.pop_front();
        }
        job();
        return true;
    }

    size_t TerrainManualExecutor::RunAll()
    {
        size_t ran = 0;
        while (RunOne())
            ++ran;
        return ran;
    }
}
