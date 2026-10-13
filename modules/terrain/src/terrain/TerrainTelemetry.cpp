/**
 * @file TerrainTelemetry.cpp
 * @author Jackey Cen
 * @brief Collects optional terrain timings and outstanding readiness requests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainTelemetry.h>

#include <chrono>
#include <cmath>
#include <utility>

namespace mir
{
    TerrainTelemetry::TerrainTelemetry(Clock clock) : mClock(std::move(clock)), mOwner(std::this_thread::get_id())
    {
        if (!mClock)
        {
            const auto start = std::chrono::steady_clock::now();
            mClock = [start] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
        }
    }

    double TerrainTelemetry::Now() const { return mClock(); }
    bool TerrainTelemetry::OnOwnerThread() const { return std::this_thread::get_id() == mOwner; }

    void TerrainTelemetry::Record(std::string_view metric, double seconds, const TerrainReleaseInfo &release,
                                  std::string_view key, bool success)
    {
        if (!std::isfinite(seconds) || seconds < 0.0)
            return;
        std::lock_guard lock(mMutex);
        mSamples.push_back({std::string(metric), std::string(key), release, Now(), seconds, success});
    }

    void TerrainTelemetry::Request(std::string_view metric, std::string_view key, const TerrainReleaseInfo &release)
    {
        std::lock_guard lock(mMutex);
        mPending.try_emplace({std::string(metric), std::string(key)}, RequestTime{release, Now()});
    }

    void TerrainTelemetry::Complete(std::string_view metric, std::string_view key)
    {
        std::lock_guard lock(mMutex);
        const auto found = mPending.find({std::string(metric), std::string(key)});
        if (found == mPending.end())
            return;
        const double now = Now();
        mSamples.push_back({std::string(metric), std::string(key), found->second.release, now,
                            now - found->second.at, true});
        mPending.erase(found);
    }

    void TerrainTelemetry::Cancel(std::string_view metric, std::string_view key)
    {
        std::lock_guard lock(mMutex);
        const auto found = mPending.find({std::string(metric), std::string(key)});
        if (found == mPending.end())
            return;
        const double now = Now();
        mSamples.push_back({std::string(metric), std::string(key), found->second.release, now,
                            now - found->second.at, false});
        mPending.erase(found);
    }

    std::vector<TerrainTimingSample> TerrainTelemetry::TakeSamples()
    {
        std::lock_guard lock(mMutex);
        return std::exchange(mSamples, {});
    }

    std::map<std::string, size_t> TerrainTelemetry::Pending() const
    {
        std::lock_guard lock(mMutex);
        std::map<std::string, size_t> out;
        for (const auto &[key, request] : mPending)
            ++out[key.first];
        return out;
    }
}
