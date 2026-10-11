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
#include <vector>

namespace mir
{
    inline constexpr const char *kDefaultIdeArgsTemplate = "--new-window {project} {file}";

    struct IdeSettings
    {
        std::string path; // empty: auto-detect (DetectDefaultIde) when used
        std::string argsTemplate = kDefaultIdeArgsTemplate;

        bool operator==(const IdeSettings &) const = default;

        static std::optional<IdeSettings> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;

        std::vector<std::string> BuildArgs(
            const std::filesystem::path &projectRoot, const std::filesystem::path &file) const;
    };

    std::optional<std::filesystem::path> DetectDefaultIde();
} // namespace mir
