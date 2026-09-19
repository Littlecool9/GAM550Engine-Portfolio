/**
 * @file AssetCache.h
 * @author Rahul Nair
 * @brief Runtime cache of cooked asset blobs - loads and parses them on
 *        demand, keyed by AssetId, resolved through an AssetManifest.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetBlob.h"
#include "assets/AssetId.h"
#include "assets/AssetManifest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    // view.content is a span into this entry's own raw buffer, so the two must
    // travel together. Moving is safe - vector's move steals the buffer, so the
    // span keeps pointing at live bytes - but copying would allocate a fresh
    // buffer and leave view.content aliasing the original, dangling as soon as
    // the source dies. Hence move-only.
    struct AssetCacheEntry
    {
        AssetCacheEntry(std::vector<std::byte> rawBytes, AssetBlobView blobView)
            : raw(std::move(rawBytes)), view(blobView)
        {
        }

        AssetCacheEntry(const AssetCacheEntry &) = delete;
        AssetCacheEntry &operator=(const AssetCacheEntry &) = delete;
        AssetCacheEntry(AssetCacheEntry &&) = default;
        AssetCacheEntry &operator=(AssetCacheEntry &&) = default;

        std::vector<std::byte> raw;
        AssetBlobView view;
    };

    class AssetCache
    {
    public:
        AssetCache(const AssetManifest *manifest, std::filesystem::path cookedRoot);

        // the entries alias their own buffers (see AssetCacheEntry), so copying
        // the whole cache would produce a map of dangling views
        AssetCache(const AssetCache &) = delete;
        AssetCache &operator=(const AssetCache &) = delete;
        AssetCache(AssetCache &&) = default;
        AssetCache &operator=(AssetCache &&) = default;

        const AssetBlobView *Load(AssetId id);
        const AssetBlobView *Get(AssetId id) const;

        void Invalidate(AssetId id);

        std::optional<std::filesystem::path> ResolvedPath(AssetId id) const;

    private:
        const AssetManifest *mManifest;
        std::filesystem::path mCookedRoot;
        std::unordered_map<uint64_t, AssetCacheEntry> mLoaded;
        std::unordered_set<uint64_t> mFailedLoads;
    };
}
