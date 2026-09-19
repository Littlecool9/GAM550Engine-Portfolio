/**
 * @file AssetManifest.h
 * @author Rahul Nair
 * @brief The cooked asset manifest - AssetId -> type/version/path, built at
 *        cook time and loaded at runtime.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetId.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kAssetManifestFileName = "manifest.blob";

    inline constexpr uint32_t kAssetManifestTypeTag = 0x4D414E46u;
    inline constexpr uint32_t kAssetManifestContentVersion = 1u;

    struct AssetManifestEntry
    {
        AssetId id{};
        uint32_t typeTag = 0;
        uint32_t contentVersion = 0;
        uint32_t pathOffset = 0;
        uint32_t pathLength = 0;
    };

    static_assert(std::is_trivially_copyable_v<AssetManifestEntry>);
    static_assert(std::is_standard_layout_v<AssetManifestEntry>);

    struct AssetManifestSourceEntry
    {
        AssetManifestEntry entry;
        std::string_view path;
    };

    std::vector<std::byte> BuildAssetManifestBlob(std::span<const AssetManifestSourceEntry> entries);

    class AssetManifest
    {
    public:
        static std::optional<AssetManifest> Parse(std::span<const std::byte> raw);
        static std::optional<AssetManifest> LoadFile(const std::filesystem::path &path);

        const AssetManifestEntry *Find(AssetId id) const;
        std::string_view PathOf(const AssetManifestEntry &entry) const;

        std::size_t Count() const { return mEntries.size(); }

    private:
        std::vector<AssetManifestEntry> mEntries;
        std::string mPathTable;
        std::unordered_map<uint64_t, uint32_t> mIndex;
    };
}
