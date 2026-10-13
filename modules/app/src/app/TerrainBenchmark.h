/**
 * @file TerrainBenchmark.h
 * @author Jackey Cen
 * @brief Drives repeatable terrain routes and records engine benchmark JSON.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/platform/Process.h>
#include <terrain/TerrainProfiledSource.h>
#include <terrain/TerrainCollisionResidency.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>

namespace mir
{
    class App;
    class RenderSystem;
    class TerrainRuntime;

    class TerrainBenchmark
    {
    public:
        static std::unique_ptr<TerrainBenchmark> Load(const std::filesystem::path &path);
        void Configure(TerrainRuntime &terrain, const std::filesystem::path &releases);
        bool Start(App &app);
        float DeltaSeconds() const;
        bool ValidationEnabled() const { return mPlan.at("validation").get<bool>(); }
        void Pace();
        void BeginFrame(App &app, uint64_t frame);
        void ApplyCameras(App &app, RenderSystem &render);
        bool EndFrame(App &app, TerrainRuntime &terrain, uint64_t frame, double cpuSeconds, double intervalSeconds);
        bool Save(bool completed);

    private:
        nlohmann::json mPlan;
        nlohmann::json mOutput;
        std::filesystem::path mConfigPath;
        std::filesystem::path mOutputPath;
        std::shared_ptr<TerrainTelemetry> mTelemetry = std::make_shared<TerrainTelemetry>();
        std::shared_ptr<TerrainReadFaults> mFaults = std::make_shared<TerrainReadFaults>();
        std::vector<TerrainCollisionSources::Id> mInterests;
        ChildProcess mPublisher;
        std::chrono::steady_clock::time_point mStart;
        size_t mRoute = 0;
        uint64_t mRouteStart = 0;
        uint64_t mFrame = 0;
        uint64_t mTotalFrames = 0;
        size_t mViewports = 1;
        bool mStarted = false;
        bool mRouteChanged = false;
        bool mFailed = false;
        WorldPosition mEye;
    };
}
