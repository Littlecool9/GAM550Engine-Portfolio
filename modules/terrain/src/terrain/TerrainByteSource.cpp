/**
 * @file TerrainByteSource.cpp
 * @author Jackey Cen
 * @brief Where one release's files are read from: a directory on disk or memory, with read counters.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainByteSource.h"

#include <core/fs/FileIO.h>

namespace mir
{
    std::optional<std::vector<std::byte>> TerrainByteSource::ReadAll(std::string_view file) const
    {
        std::optional<std::vector<std::byte>> read = DoReadAll(file);
        Count(read);
        return read;
    }

    std::optional<std::vector<std::byte>> TerrainByteSource::ReadRange(std::string_view file, uint64_t offset,
                                                                       size_t length) const
    {
        std::optional<std::vector<std::byte>> read = DoReadRange(file, offset, length);
        Count(read);
        return read;
    }

    std::optional<uint64_t> TerrainByteSource::Size(std::string_view file) const
    {
        return DoSize(file);
    }

    TerrainByteStats TerrainByteSource::Stats() const
    {
        return {mReads.load(), mBytes.load()};
    }

    void TerrainByteSource::ResetStats()
    {
        mReads = 0;
        mBytes = 0;
    }

    void TerrainByteSource::Count(const std::optional<std::vector<std::byte>> &read) const
    {
        mReads.fetch_add(1);
        if (read)
            mBytes.fetch_add(read->size());
    }

    std::string TerrainFileSource::Describe() const
    {
        return mDirectory.generic_string();
    }

    std::optional<std::vector<std::byte>> TerrainFileSource::DoReadAll(std::string_view file) const
    {
        return ReadBytes(mDirectory / file);
    }

    std::optional<std::vector<std::byte>> TerrainFileSource::DoReadRange(std::string_view file, uint64_t offset,
                                                                         size_t length) const
    {
        return ReadByteRange(mDirectory / file, offset, length);
    }

    std::optional<uint64_t> TerrainFileSource::DoSize(std::string_view file) const
    {
        return FileSize(mDirectory / file);
    }

    void TerrainMemorySource::Put(std::string file, std::vector<std::byte> bytes)
    {
        std::lock_guard lock(mMutex);
        mFiles.insert_or_assign(std::move(file), std::move(bytes));
    }

    void TerrainMemorySource::Remove(std::string_view file)
    {
        std::lock_guard lock(mMutex);
        if (const auto it = mFiles.find(file); it != mFiles.end())
            mFiles.erase(it);
    }

    std::optional<std::vector<std::byte>> TerrainMemorySource::Get(std::string_view file) const
    {
        std::lock_guard lock(mMutex);
        const auto it = mFiles.find(file);
        if (it == mFiles.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<TerrainMemorySource::Request> TerrainMemorySource::Requests() const
    {
        std::lock_guard lock(mMutex);
        return mRequests;
    }

    void TerrainMemorySource::ClearRequests()
    {
        std::lock_guard lock(mMutex);
        mRequests.clear();
    }

    std::optional<std::vector<std::byte>> TerrainMemorySource::DoReadAll(std::string_view file) const
    {
        std::lock_guard lock(mMutex);
        const auto it = mFiles.find(file);
        mRequests.push_back({std::string(file), 0, it == mFiles.end() ? 0 : it->second.size()});
        if (it == mFiles.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<std::vector<std::byte>> TerrainMemorySource::DoReadRange(std::string_view file, uint64_t offset,
                                                                           size_t length) const
    {
        std::lock_guard lock(mMutex);
        mRequests.push_back({std::string(file), offset, length});
        const auto it = mFiles.find(file);
        if (it == mFiles.end())
            return std::nullopt;
        const std::vector<std::byte> &bytes = it->second;
        if (offset > bytes.size() || length > bytes.size() - offset)
            return std::nullopt;
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        return std::vector<std::byte>(begin, begin + static_cast<std::ptrdiff_t>(length));
    }

    std::optional<uint64_t> TerrainMemorySource::DoSize(std::string_view file) const
    {
        std::lock_guard lock(mMutex);
        const auto it = mFiles.find(file);
        if (it == mFiles.end())
            return std::nullopt;
        return it->second.size();
    }
}
