/**
 * @file EditorPreferences.h
 * @author Jackey Cen
 * @brief Per-user editor preferences, stored outside any project.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <string>

namespace mir
{
    enum class BackgroundFit
    {
        Cover,
        Contain,
        Stretch,
    };

    struct EditorPreferences
    {
        bool backgroundEnabled = false;
        // file name inside EditorPreferencesDir(); empty is no image
        std::string backgroundFile;
        // how much of the image shows through the panels, 0..1
        float backgroundStrength = 0.2f;
        BackgroundFit backgroundFit = BackgroundFit::Cover;

        // defaults for a missing file and for any missing or unusable field
        static EditorPreferences LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };

    // %LOCALAPPDATA%/Mjolnir/editor on Windows, next to the executable elsewhere
    std::filesystem::path EditorPreferencesDir();
    std::filesystem::path EditorPreferencesPath();
}
