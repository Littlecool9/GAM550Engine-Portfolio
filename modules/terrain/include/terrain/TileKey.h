/**
 * @file TileKey.h
 * @author Jackey Cen
 * @brief Logical terrain tile address: asset id plus signed tile coordinates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace mir
{
    // Signed tile coordinates in a terrain asset's local grid. Independent of
    // files, storage chunks and render patches.
    struct TileCoord
    {
        int32_t x = 0;
        int32_t z = 0;

        constexpr bool operator==(const TileCoord &) const = default;
    };

    // Names shared tile data: every placement of the same asset uses the same keys.
    struct TileKey
    {
        AssetId asset;
        TileCoord coord;

        constexpr bool operator==(const TileKey &) const = default;
    };
}

template <>
struct std::hash<mir::TileCoord>
{
    size_t operator()(const mir::TileCoord &coord) const noexcept
    {
        const uint64_t packed = (uint64_t(uint32_t(coord.x)) << 32) | uint32_t(coord.z);
        return std::hash<uint64_t>{}(packed);
    }
};

template <>
struct std::hash<mir::TileKey>
{
    size_t operator()(const mir::TileKey &key) const noexcept
    {
        const size_t asset = std::hash<uint64_t>{}(key.asset.value);
        return asset ^ (std::hash<mir::TileCoord>{}(key.coord) + 0x9e3779b97f4a7c15ull + (asset << 6) + (asset >> 2));
    }
};
