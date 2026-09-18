/**
 * @file GameProject.cpp
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameProject.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <serial/JsonFile.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace mir
{
    std::optional<std::string> ReadDefaultGame()
    {
        const std::filesystem::path path = ExecutableDir() / kEngineManifestFileName;

        const std::optional<nlohmann::json> json = ReadJsonFile(path);
        if (!json)
        {
            MIR_LOG_ERROR("no --game given and could not read '{}'", path.string());
            return std::nullopt;
        }

        const auto it = json->find("defaultGame");
        if (it == json->end() || !it->is_string() || it->get<std::string>().empty())
        {
            MIR_LOG_ERROR("no --game given and '{}' names no defaultGame", path.string());
            return std::nullopt;
        }

        return it->get<std::string>();
    }

    std::optional<std::filesystem::path> NormalizeGameDir(const std::string &requested)
    {
        const std::filesystem::path &exeDir = ExecutableDir();

        std::filesystem::path dir = std::filesystem::path(requested).lexically_normal();
        if (dir.is_absolute())
            dir = dir.lexically_relative(exeDir.lexically_normal());

        // lexically_normal keeps a trailing separator as an empty filename
        if (!dir.empty() && dir.filename().empty())
            dir = dir.parent_path();

        if (dir.empty() || dir == "." || dir.has_root_path() || *dir.begin() == "..")
        {
            MIR_LOG_ERROR("game directory '{}' is not inside '{}'", requested, exeDir.string());
            return std::nullopt;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(exeDir / dir, ec))
        {
            MIR_LOG_ERROR("game directory '{}' does not exist", (exeDir / dir).string());
            return std::nullopt;
        }

        return dir;
    }

    std::optional<GameProject> LoadGameProject(const std::filesystem::path &gameDir)
    {
        const std::filesystem::path root = ExecutableDir() / gameDir;
        const std::filesystem::path manifestPath = root / kGameManifestFileName;

        std::error_code manifestEc;
        if (!std::filesystem::is_regular_file(manifestPath, manifestEc))
        {
            MIR_LOG_ERROR("LoadGameProject: no {} in '{}', not a game directory", kGameManifestFileName, root.string());
            return std::nullopt;
        }

        GameProject project;
        project.mDir = gameDir.generic_string();
        project.mName = gameDir.filename().string();

        // assetsRoot default must match CMakeLists.txt
        std::string assetsRoot = "assets";
        std::string sceneDir = "scenes/default";

        // A malformed manifest keeps the defaults; ReadJsonFile logged why.
        if (const std::optional<nlohmann::json> json = ReadJsonFile(manifestPath); json && json->is_object())
        {
            project.mName = json->value("name", project.mName);
            project.mTitle = json->value("title", project.mTitle);
            assetsRoot = json->value("assetsRoot", assetsRoot);
            sceneDir = json->value("sceneDir", sceneDir);
        }

        if (project.mName.empty())
            project.mName = gameDir.filename().string();

        // normalized like AssetCooker's NormalizeRoot, so ids match
        std::string idRoot = (gameDir / assetsRoot).lexically_normal().generic_string();
        while (!idRoot.empty() && idRoot.back() == '/')
            idRoot.pop_back();

        project.mAssetIdRoot = std::move(idRoot);
        project.mSceneDir = root / sceneDir;
        project.mProjectSettingsPath = root / kProjectSettingsFileName;
        return project;
    }

    std::optional<GameProject> ResolveGameProject(const std::string &requested)
    {
        const std::optional<std::filesystem::path> gameDir = NormalizeGameDir(requested);
        if (!gameDir.has_value())
            return std::nullopt;

        MIR_LOG_INFO("Loading game '{}'", gameDir->generic_string());
        return LoadGameProject(*gameDir);
    }

    std::vector<GameProject> DiscoverGameProjects()
    {
        std::vector<GameProject> projects;

        const std::filesystem::path gamesDir = ExecutableDir() / "games";

        std::error_code ec;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(gamesDir, ec))
        {
            std::error_code entryEc;
            if (!entry.is_directory(entryEc) ||
                !std::filesystem::is_regular_file(entry.path() / kGameManifestFileName, entryEc))
                continue;

            if (std::optional<GameProject> project = LoadGameProject(std::filesystem::path("games") / entry.path().filename()))
                projects.push_back(std::move(*project));
        }

        if (ec)
            MIR_LOG_WARN("DiscoverGameProjects: could not list '{}': {}", gamesDir.string(), ec.message());

        std::sort(projects.begin(), projects.end(),
            [](const GameProject &a, const GameProject &b)
            {
                return a.mDir < b.mDir;
            });
        return projects;
    }
}
