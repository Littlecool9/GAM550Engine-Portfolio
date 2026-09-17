/**
 * @file AssetManifest.cpp
 * @author Rahul Nair
 * @brief The cooked asset manifest - AssetId -> type/version/path, built at
 *        cook time and loaded at runtime.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "assets/AssetManifest.h"

#include "assets/AssetBlob.h"
#include "assets/AssetFileIo.h"
#include "core/log/Log.h"

#include <cstring>

namespace mir
{
    std::vector<std::byte> BuildAssetManifestBlob(std::span<const AssetManifestSourceEntry> entries)
    {
        std::vector<AssetManifestEntry> flatEntries;
        flatEntries.reserve(entries.size());
        std::string pathTable;

        for (const AssetManifestSourceEntry &src : entries)
        {
            AssetManifestEntry entry = src.entry;
            entry.pathOffset = static_cast<uint32_t>(pathTable.size());
            entry.pathLength = static_cast<uint32_t>(src.path.size());
            pathTable.append(src.path);
            flatEntries.push_back(entry);
        }

        const uint64_t entryCount = flatEntries.size();
        const std::size_t entriesBytes = flatEntries.size() * sizeof(AssetManifestEntry);

        std::vector<std::byte> content(sizeof(entryCount) + entriesBytes + pathTable.size());
        std::size_t offset = 0;

        std::memcpy(content.data() + offset, &entryCount, sizeof(entryCount));
        offset += sizeof(entryCount);

        if (!flatEntries.empty())
        {
            std::memcpy(content.data() + offset, flatEntries.data(), entriesBytes);
            offset += entriesBytes;
        }

        if (!pathTable.empty())
            std::memcpy(content.data() + offset, pathTable.data(), pathTable.size());

        return BuildAssetBlob(kAssetManifestTypeTag, kAssetManifestContentVersion, content);
    }

    std::optional<AssetManifest> AssetManifest::Parse(std::span<const std::byte> raw)
    {
        const std::optional<AssetBlobView> blob = ParseAssetBlob(raw);
        if (!blob.has_value())
            return std::nullopt;

        if (blob->header.typeTag != kAssetManifestTypeTag)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: type tag mismatch, expected {}, got {}",
                          kAssetManifestTypeTag, blob->header.typeTag);
            return std::nullopt;
        }

        // AssetCache::Load checks this for every ordinary asset; the manifest is
        // the one blob that must be trustworthy, so it cannot be the one that
        // skips the check. Without it an old manifest memcpys its stale entry
        // layout into the current struct and yields garbage ids and offsets.
        if (blob->header.contentVersion != kAssetManifestContentVersion)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: unsupported content version {}, expected {}",
                          blob->header.contentVersion, kAssetManifestContentVersion);
            return std::nullopt;
        }

        const std::span<const std::byte> content = blob->content;
        if (content.size() < sizeof(uint64_t))
        {
            MIR_LOG_ERROR("AssetManifest::Parse: content too small for entry count");
            return std::nullopt;
        }

        uint64_t entryCount = 0;
        std::memcpy(&entryCount, content.data(), sizeof(entryCount));

        const std::size_t availableForEntries = content.size() - sizeof(entryCount);
        const std::size_t maxEntries = availableForEntries / sizeof(AssetManifestEntry);
        if (entryCount > maxEntries)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: content too small for {} entries", entryCount);
            return std::nullopt;
        }

        const std::size_t entriesBytes = static_cast<std::size_t>(entryCount) * sizeof(AssetManifestEntry);

        AssetManifest manifest;
        manifest.mEntries.resize(entryCount);
        if (entryCount > 0)
            std::memcpy(manifest.mEntries.data(), content.data() + sizeof(entryCount), entriesBytes);

        const std::span<const std::byte> pathBytes = content.subspan(sizeof(entryCount) + entriesBytes);
        manifest.mPathTable.assign(reinterpret_cast<const char *>(pathBytes.data()), pathBytes.size());

        manifest.mIndex.reserve(manifest.mEntries.size());
        for (uint32_t i = 0; i < manifest.mEntries.size(); ++i)
        {
            const AssetManifestEntry &entry = manifest.mEntries[i];
            if (static_cast<std::size_t>(entry.pathOffset) + entry.pathLength > manifest.mPathTable.size())
            {
                MIR_LOG_ERROR("AssetManifest::Parse: entry {} path range out of bounds", i);
                return std::nullopt;
            }
            // emplace does not overwrite, so a duplicate would leave the second
            // entry permanently unreachable with no diagnostic - which is exactly
            // what an id collision looks like. Reject instead.
            if (!manifest.mIndex.emplace(entry.id.value, i).second)
            {
                MIR_LOG_ERROR("AssetManifest::Parse: duplicate asset id {:#x} at entry {}", entry.id.value, i);
                return std::nullopt;
            }
        }

        return manifest;
    }

    std::optional<AssetManifest> AssetManifest::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<std::vector<std::byte>> bytes = ReadFileBytes(path);
        if (!bytes.has_value())
            return std::nullopt;

        return Parse(*bytes);
    }

    const AssetManifestEntry *AssetManifest::Find(AssetId id) const
    {
        auto it = mIndex.find(id.value);
        if (it == mIndex.end())
            return nullptr;
        return &mEntries[it->second];
    }

    std::string_view AssetManifest::PathOf(const AssetManifestEntry &entry) const
    {
        return std::string_view(mPathTable).substr(entry.pathOffset, entry.pathLength);
    }
}
