/**
 * @file PackInternal.h
 * @author Jackey Cen
 * @brief Helpers shared by the terrain pack writer, parser and release reader.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainPackFormat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mir::detail
{
    inline constexpr std::array<std::byte, 4> kIndexMagic{std::byte{'M'}, std::byte{'J'}, std::byte{'T'},
                                                          std::byte{'I'}};
    inline constexpr std::array<std::byte, 4> kPackMagic{std::byte{'M'}, std::byte{'J'}, std::byte{'T'},
                                                         std::byte{'P'}};

    inline bool LessZX(int32_t ax, int32_t az, int32_t bx, int32_t bz)
    {
        return az != bz ? az < bz : ax < bx;
    }

    inline std::string Describe(TileCoord tile)
    {
        return std::format("({}, {})", tile.x, tile.z);
    }

    inline std::string Describe(TerrainChunkCoord chunk)
    {
        return std::format("({}, {})", chunk.x, chunk.z);
    }

    inline std::string Hex(uint64_t value)
    {
        return std::format("{:016x}", value);
    }

    template <typename T>
    TerrainResult<T> Failure(TerrainPackErrorCode code, std::string message)
    {
        TerrainResult<T> result;
        result.error = {code, std::move(message)};
        return result;
    }

    template <typename... Args>
    TerrainPackError Error(TerrainPackErrorCode code, std::format_string<Args...> format, Args &&...args)
    {
        return {code, std::format(format, std::forward<Args>(args)...)};
    }

    template <typename T, typename... Args>
    TerrainResult<T> Fail(TerrainPackErrorCode code, std::format_string<Args...> format, Args &&...args)
    {
        return Failure<T>(code, std::format(format, std::forward<Args>(args)...));
    }

    template <typename T>
    TerrainResult<T> Failure(TerrainPackError error, std::string_view prefix)
    {
        return Failure<T>(error.code, std::string(prefix) + error.message);
    }

    uint64_t HeightsHashOf(std::span<const TerrainTileRecord> records);
}
