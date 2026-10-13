/**
 * @file TerrainProfiledSource.cpp
 * @author Jackey Cen
 * @brief Measures terrain byte reads and injects recoverable benchmark range failures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainProfiledSource.h>

#include <utility>

namespace mir
{
    TerrainProfiledSource::TerrainProfiledSource(std::shared_ptr<TerrainByteSource> source,
        std::shared_ptr<TerrainTelemetry> telemetry, std::shared_ptr<TerrainReadFaults> faults, TerrainReleaseInfo release)
        : mSource(std::move(source)), mTelemetry(std::move(telemetry)), mFaults(std::move(faults)), mRelease(release)
    {
    }

    std::optional<std::vector<std::byte>> TerrainProfiledSource::DoReadAll(std::string_view file) const
    {
        const double start = mTelemetry->Now();
        auto bytes = mSource->ReadAll(file);
        const std::string_view metric = mTelemetry->OnOwnerThread() ? "main_metadata_io" : "verification_io";
        mTelemetry->Record(metric, mTelemetry->Now() - start, mRelease, file, bytes.has_value());
        return bytes;
    }

    std::optional<std::vector<std::byte>> TerrainProfiledSource::DoReadRange(std::string_view file, uint64_t offset,
                                                                         size_t length) const
    {
        const double start = mTelemetry->Now();
        const bool main = mTelemetry->OnOwnerThread();
        if (main)
            ++mFaults->mainThreadRanges;
        uint64_t remaining = mFaults->remaining.load();
        while (remaining && !mFaults->remaining.compare_exchange_weak(remaining, remaining - 1)) {}
        std::optional<std::vector<std::byte>> bytes;
        if (remaining)
            ++mFaults->injected;
        else
            bytes = mSource->ReadRange(file, offset, length);
        mTelemetry->Record(main ? "main_thread_io" : "io_range", mTelemetry->Now() - start,
                            mRelease, file, bytes.has_value());
        return bytes;
    }
}
