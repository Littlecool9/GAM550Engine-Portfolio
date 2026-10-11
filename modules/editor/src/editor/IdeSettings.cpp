/**
 * @file IdeSettings.cpp
 * @author Rahul Nair
 * @brief Loads/saves the "ide" section of project_settings.json, and finds a
 *        system VS Code install to default to.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/IdeSettings.h>

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <cstdlib>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    std::optional<IdeSettings> IdeSettings::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return std::nullopt;

        IdeSettings settings;

        const auto it = root->find("ide");
        if (it == root->end() || !it->is_object())
            return settings;

        settings.path = it->value("path", std::string{});
        return settings;
    }

    bool IdeSettings::SaveFile(const std::filesystem::path &filePath) const
    {
        return UpdateJsonFile(filePath, [this](json &root) { root["ide"] = json{ { "path", path } }; });
    }

#if defined(_WIN32)
    std::optional<std::filesystem::path> DetectDefaultIde()
    {
        struct EnvRoot
        {
            const char *variable;
            const char *suffix;
        };
        // Checked in the order a VS Code installer is most likely to have
        // used: per-user default, then the two system-wide locations.
        constexpr EnvRoot kCandidates[] = {
            { "LOCALAPPDATA", "Programs\\Microsoft VS Code\\Code.exe" },
            { "ProgramFiles", "Microsoft VS Code\\Code.exe" },
            { "ProgramFiles(x86)", "Microsoft VS Code\\Code.exe" },
        };

        for (const EnvRoot &candidate : kCandidates)
        {
            const char *root = std::getenv(candidate.variable);
            if (root == nullptr)
                continue;
            std::error_code error;
            if (std::filesystem::path path = std::filesystem::path(root) / candidate.suffix;
                std::filesystem::is_regular_file(path, error))
                return path;
        }
        return std::nullopt;
    }
#elif defined(__linux__)
    std::optional<std::filesystem::path> DetectDefaultIde()
    {
        constexpr const char *kCandidates[] = {
            "/usr/bin/code",
            "/usr/share/code/bin/code",
            "/snap/bin/code",
        };
        for (const char *candidate : kCandidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
                return std::filesystem::path(candidate);
        }
        return std::nullopt;
    }
#else
    std::optional<std::filesystem::path> DetectDefaultIde() { return std::nullopt; }
#endif
} // namespace mir
