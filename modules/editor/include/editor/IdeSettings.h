/**
 * @file IdeSettings.h
 * @author Rahul Nair
 * @brief Project Settings' "External Tools" section - which IDE the Content
 *        Browser opens a script in.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mir
{
    struct IdeSettings
    {
        std::string path; // empty: auto-detect (DetectDefaultIde) when used

        bool operator==(const IdeSettings &) const = default;

        static std::optional<IdeSettings> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };

    std::optional<std::filesystem::path> DetectDefaultIde();
} // namespace mir
