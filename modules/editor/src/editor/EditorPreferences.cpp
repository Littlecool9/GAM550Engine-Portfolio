/**
 * @file EditorPreferences.cpp
 * @author Jackey Cen
 * @brief Per-user editor preferences, stored outside any project.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorPreferences.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char *kFitNames[] = {"cover", "contain", "stretch"};
    }

    EditorPreferences EditorPreferences::LoadFile(const std::filesystem::path &path)
    {
        EditorPreferences prefs;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return prefs;

        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
        {
            MIR_LOG_WARN("Editor preferences '{}' are unreadable; using defaults", path.string());
            return prefs;
        }

        const auto background = root->find("background");
        if (background == root->end() || !background->is_object())
            return prefs;

        if (const auto it = background->find("enabled"); it != background->end() && it->is_boolean())
            prefs.backgroundEnabled = it->get<bool>();

        // only a bare file name, so the image always stays inside the preferences directory
        if (const auto it = background->find("file"); it != background->end() && it->is_string())
        {
            const std::string file = it->get<std::string>();
            if (file != "." && file != ".." && file.find_first_of("/\\:") == std::string::npos)
                prefs.backgroundFile = file;
        }

        if (const auto it = background->find("strength"); it != background->end() && it->is_number())
        {
            const float strength = it->get<float>();
            if (std::isfinite(strength))
                prefs.backgroundStrength = std::clamp(strength, 0.0f, 1.0f);
        }

        if (const auto it = background->find("fit"); it != background->end() && it->is_string())
        {
            const std::string fit = it->get<std::string>();
            for (size_t i = 0; i < std::size(kFitNames); ++i)
            {
                if (fit == kFitNames[i])
                    prefs.backgroundFit = static_cast<BackgroundFit>(i);
            }
        }

        return prefs;
    }

    bool EditorPreferences::SaveFile(const std::filesystem::path &path) const
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            MIR_LOG_ERROR("Cannot create '{}': {}", path.parent_path().string(), ec.message());
            return false;
        }

        const json root{{"background",
                         {{"enabled", backgroundEnabled},
                          {"file", backgroundFile},
                          {"strength", backgroundStrength},
                          {"fit", kFitNames[static_cast<size_t>(backgroundFit)]}}}};
        return WriteJsonFile(path, root);
    }

    std::filesystem::path EditorPreferencesDir()
    {
#if defined(_WIN32)
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        {
            std::filesystem::path dir = std::filesystem::path(localAppData) / "Mjolnir" / "editor";
            CoTaskMemFree(localAppData);
            return dir;
        }
        CoTaskMemFree(localAppData);
#endif
        return ExecutableDir() / "editor";
    }

    std::filesystem::path EditorPreferencesPath() { return EditorPreferencesDir() / "editor_preferences.json"; }
}
