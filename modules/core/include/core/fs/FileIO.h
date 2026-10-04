/**
 * @file FileIO.h
 * @author Sumin Park
 * @brief  For byte level file read / write
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // file as byte
    std::optional<std::vector<std::byte>> ReadBytes(const std::filesystem::path &path);

    // first count amount of bytes of file
    std::optional<std::vector<std::byte>> ReadFirstBytes(const std::filesystem::path &path, std::size_t count);

    // Reads only [offset, offset + length); logs and fails if the range runs past the file.
    std::optional<std::vector<std::byte>> ReadByteRange(const std::filesystem::path &path, std::uint64_t offset,
                                                        std::size_t length);

    // Logs and fails if the path is missing or not a regular file.
    std::optional<std::uint64_t> FileSize(const std::filesystem::path &path);

    // file as text
    std::optional<std::string> ReadText(const std::filesystem::path &path);

    // for tools. for user authored files(settings, scenes etc), use WriteBytesAtomic
    bool WriteBytes(const std::filesystem::path &path, std::span<const std::byte> bytes);

    bool WriteText(const std::filesystem::path &path, std::string_view text);

    // safe file writing - for user authored files
    bool WriteBytesAtomic(const std::filesystem::path &path, std::span<const std::byte> bytes);

    bool WriteTextAtomic(const std::filesystem::path &path, std::string_view text);
}
