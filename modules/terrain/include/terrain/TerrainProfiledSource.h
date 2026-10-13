/**
 * @file TerrainProfiledSource.h
 * @author Jackey Cen
 * @brief Measures terrain byte reads and injects recoverable benchmark range failures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainByteSource.h>
#include <terrain/TerrainTelemetry.h>

#include <memory>

namespace mir
{
    struct TerrainReadFaults
    {
        std::atomic<uint64_t> remaining{0};
        std::atomic<uint64_t> injected{0};
        std::atomic<uint64_t> mainThreadRanges{0};
    };

    class TerrainProfiledSource final : public TerrainByteSource
    {
    public:
        TerrainProfiledSource(std::shared_ptr<TerrainByteSource> source, std::shared_ptr<TerrainTelemetry> telemetry,
                              std::shared_ptr<TerrainReadFaults> faults, TerrainReleaseInfo release);
        std::string Describe() const override { return mSource->Describe(); }

    protected:
        std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const override;
        std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                        size_t length) const override;
        std::optional<uint64_t> DoSize(std::string_view file) const override { return mSource->Size(file); }

    private:
        std::shared_ptr<TerrainByteSource> mSource;
        std::shared_ptr<TerrainTelemetry> mTelemetry;
        std::shared_ptr<TerrainReadFaults> mFaults;
        TerrainReleaseInfo mRelease;
    };
}
