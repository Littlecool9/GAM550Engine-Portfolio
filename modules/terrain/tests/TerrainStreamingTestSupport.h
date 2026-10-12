/**
 * @file TerrainStreamingTestSupport.h
 * @author Jackey Cen
 * @brief Streaming settings, a controllable byte source and a manual clock for the terrain streaming tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainByteSource.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>

#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mir::test
{
    inline TerrainStreamingSettings StreamingSettings(uint64_t budgetBytes,
                                                      TerrainResidencyMode mode = TerrainResidencyMode::Auto)
    {
        TerrainStreamingSettings settings;
        settings.cpu.budgetBytes = budgetBytes;
        settings.io.workerThreads = 2;
        settings.io.maxInstallsPerFrame = 64;
        settings.retry.maxAttempts = 3;
        settings.retry.firstDelaySeconds = 0.001;
        settings.retry.maxDelaySeconds = 0.004;
        settings.retry.failedHoldSeconds = 1.0;
        settings.residency.mode = mode;
        settings.residency.queryLeaseSeconds = 1.0;
        settings.view.refineErrorPixels = 1.0;
        settings.view.mergeErrorPixels = 0.75;
        settings.view.prefetchErrorPixels = 0.5;
        settings.view.prefetchBudgetBytes = uint64_t(1) << 24;
        settings.gpu.budgetBytes = uint64_t(1) << 28;
        settings.gpu.uploadBytesPerFrame = uint64_t(1) << 22;
        settings.gpu.stagingBytes = uint64_t(1) << 24;
        settings.gpu.meshRetainSeconds = 1.0;
        REQUIRE(ValidateTerrainStreamingSettings(settings).empty());
        return settings;
    }

    // What scene and script tests resolve through: everything resident, read by a real worker pool.
    inline TerrainStreamer &EmplaceTestStreamer(World &world)
    {
        return world.EmplaceResource<TerrainStreamer>(StreamingSettings(uint64_t(1) << 30),
                                                      std::make_shared<TerrainWorkerPool>(2));
    }

    struct ManualClock
    {
        std::shared_ptr<double> now = std::make_shared<double>(0.0);

        TerrainStreamer::Clock Function() const
        {
            return [now = now] { return *now; };
        }
        void Advance(double seconds) { *now += seconds; }
    };

    // Wraps an in-memory release: holds reads until resumed, fails or corrupts chosen files, records the
    // reading threads.
    class ControlledSource final : public TerrainByteSource
    {
    public:
        explicit ControlledSource(std::shared_ptr<TerrainMemorySource> inner) : mInner(std::move(inner)) {}

        TerrainMemorySource &Inner() { return *mInner; }

        void Hold()
        {
            std::lock_guard lock(mMutex);
            mHeld = true;
        }

        void Resume()
        {
            {
                std::lock_guard lock(mMutex);
                mHeld = false;
            }
            mResumed.notify_all();
        }

        // Range reads of file return nothing the next times calls.
        void FailNext(std::string file, uint32_t times)
        {
            std::lock_guard lock(mMutex);
            mFailures[std::move(file)] = times;
        }

        // Range reads covering the byte at offset return it flipped.
        void Corrupt(std::string file, uint64_t offset)
        {
            std::lock_guard lock(mMutex);
            mCorrupt.insert({std::move(file), offset});
        }

        size_t BlockedReads() const
        {
            std::lock_guard lock(mMutex);
            return mBlocked;
        }

        std::set<std::thread::id> ReaderThreads() const
        {
            std::lock_guard lock(mMutex);
            return mThreads;
        }

        std::vector<TerrainMemorySource::Request> RangeRequests() const
        {
            std::lock_guard lock(mMutex);
            return mRanges;
        }

        std::string Describe() const override { return "<controlled>"; }

    protected:
        std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const override
        {
            return mInner->ReadAll(file);
        }

        std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                          size_t length) const override
        {
            {
                std::unique_lock lock(mMutex);
                mThreads.insert(std::this_thread::get_id());
                mRanges.push_back({std::string(file), offset, length});
                ++mBlocked;
                mResumed.wait(lock, [this] { return !mHeld; });
                --mBlocked;
                if (const auto failure = mFailures.find(file); failure != mFailures.end() && failure->second > 0)
                {
                    --failure->second;
                    return std::nullopt;
                }
            }
            std::optional<std::vector<std::byte>> bytes = mInner->ReadRange(file, offset, length);
            std::lock_guard lock(mMutex);
            if (bytes)
                for (const auto &[corruptFile, at] : mCorrupt)
                    if (corruptFile == file && at >= offset && at < offset + length)
                        (*bytes)[size_t(at - offset)] ^= std::byte{0x40};
            return bytes;
        }

        std::optional<uint64_t> DoSize(std::string_view file) const override { return mInner->Size(file); }

    private:
        std::shared_ptr<TerrainMemorySource> mInner;
        mutable std::mutex mMutex;
        mutable std::condition_variable mResumed;
        bool mHeld = false;
        mutable size_t mBlocked = 0;
        mutable std::map<std::string, uint32_t, std::less<>> mFailures;
        std::set<std::pair<std::string, uint64_t>> mCorrupt;
        mutable std::set<std::thread::id> mThreads;
        mutable std::vector<TerrainMemorySource::Request> mRanges;
    };

    struct ControlledRelease
    {
        std::shared_ptr<ControlledSource> source;
        std::shared_ptr<TerrainResidentRelease> release;
    };

    inline ControlledRelease OpenControlled(const TerrainEncodedRelease &encoded)
    {
        auto source = std::make_shared<ControlledSource>(MakeTerrainMemorySource(encoded));
        return {source, TerrainResidentRelease::Create(OpenOk(TerrainReleaseHandle::Open(source)))};
    }

    // Updates until done() holds; fails the test instead of hanging.
    template <typename Done>
    void UpdateUntil(TerrainStreamer &streamer, Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;)
        {
            streamer.Update();
            if (done())
                return;
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            streamer.WaitForCompletion(std::chrono::milliseconds(5));
        }
    }
}
