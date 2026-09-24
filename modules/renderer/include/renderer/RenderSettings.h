/**
 * @file RenderSettings.h
 * @author Rahul Nair
 * @brief Project Settings' "rendering" section. A World resource, loaded and
 *        saved the same way InputMap is (see input/InputMap.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>

namespace mir
{
    struct RenderSettings
    {
        bool rayTracing = false;
        bool fog = false;

        // nullopt if the file is missing or unreadable; a file with no
        // "rendering" section loads as defaults.
        static std::optional<RenderSettings> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };
}
