/**
 * @file TerrainTelemetry.h
 * @author Jackey Cen
 * @brief Collects optional terrain timings and outstanding readiness requests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mir
{
    struct TerrainTimingSample
    {
        std::string metric;
        std::string key;
        TerrainReleaseInfo release;
        double atSeconds = 0.0;
        double seconds = 0.0;
        bool success = true;
    };

    class TerrainTelemetry
    {
    public:
        using Clock = std::function<double()>;

        explicit TerrainTelemetry(Clock clock = {});
        double Now() const;
        bool OnOwnerThread() const;
        void Record(std::string_view metric, double seconds, const TerrainReleaseInfo &release = {},
                    std::string_view key = {}, bool success = true);
        void Request(std::string_view metric, std::string_view key, const TerrainReleaseInfo &release);
        void Complete(std::string_view metric, std::string_view key);
        void Cancel(std::string_view metric, std::string_view key);
        std::vector<TerrainTimingSample> TakeSamples();
        std::map<std::string, size_t> Pending() const;

    private:
        struct RequestTime
        {
            TerrainReleaseInfo release;
            double at = 0.0;
        };

        Clock mClock;
        std::thread::id mOwner;
        mutable std::mutex mMutex;
        std::map<std::pair<std::string, std::string>, RequestTime> mPending;
        std::vector<TerrainTimingSample> mSamples;
    };
}
