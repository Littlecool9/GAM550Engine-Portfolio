/**
 * @file TerrainByteSource.h
 * @author Jackey Cen
 * @brief Where one release's files are read from: a directory on disk or memory, with read counters.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mir
{
    struct TerrainByteStats
    {
        uint64_t reads = 0;
        uint64_t bytes = 0;
    };

    // Files are named relative to one release directory. Every read is
    // counted, so a caller can prove a lookup touched only its tile's range.
    class TerrainByteSource
    {
    public:
        virtual ~TerrainByteSource() = default;

        std::optional<std::vector<std::byte>> ReadAll(std::string_view file) const;
        std::optional<std::vector<std::byte>> ReadRange(std::string_view file, uint64_t offset,
                                                        size_t length) const;
        // A stat, not counted as a read.
        std::optional<uint64_t> Size(std::string_view file) const;

        TerrainByteStats Stats() const;
        void ResetStats();

        // Where the files live, for diagnostics.
        virtual std::string Describe() const = 0;

    protected:
        virtual std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const = 0;
        virtual std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                                  size_t length) const = 0;
        virtual std::optional<uint64_t> DoSize(std::string_view file) const = 0;

    private:
        void Count(const std::optional<std::vector<std::byte>> &read) const;

        mutable std::atomic<uint64_t> mReads{0};
        mutable std::atomic<uint64_t> mBytes{0};
    };

    // Reads through core FileIO, which logs a missing file or a short read.
    // Opens a file per read and keeps nothing open.
    class TerrainFileSource final : public TerrainByteSource
    {
    public:
        explicit TerrainFileSource(std::filesystem::path directory) : mDirectory(std::move(directory)) {}

        const std::filesystem::path &Directory() const { return mDirectory; }
        std::string Describe() const override;

    protected:
        std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const override;
        std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                          size_t length) const override;
        std::optional<uint64_t> DoSize(std::string_view file) const override;

    private:
        std::filesystem::path mDirectory;
    };

    // Files held in memory, for tests and preloaded fixtures. Logs every
    // request so a test can check the exact range a lookup asked for.
    class TerrainMemorySource final : public TerrainByteSource
    {
    public:
        struct Request
        {
            std::string file;
            uint64_t offset = 0;
            uint64_t length = 0;
        };

        void Put(std::string file, std::vector<std::byte> bytes);
        void Remove(std::string_view file);
        // Copy of a file's bytes; nullopt if absent.
        std::optional<std::vector<std::byte>> Get(std::string_view file) const;

        std::vector<Request> Requests() const;
        void ClearRequests();

        std::string Describe() const override { return "<memory>"; }

    protected:
        std::optional<std::vector<std::byte>> DoReadAll(std::string_view file) const override;
        std::optional<std::vector<std::byte>> DoReadRange(std::string_view file, uint64_t offset,
                                                          size_t length) const override;
        std::optional<uint64_t> DoSize(std::string_view file) const override;

    private:
        mutable std::mutex mMutex;
        std::map<std::string, std::vector<std::byte>, std::less<>> mFiles;
        mutable std::vector<Request> mRequests;
    };
}
