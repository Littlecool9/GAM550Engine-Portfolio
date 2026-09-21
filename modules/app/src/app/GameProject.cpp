/**
 * @file GameProject.cpp
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameProject.h>

#include <assets/AssetFileIo.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/RenderSettings.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        std::filesystem::path ProjectRoot()
        {
            std::ifstream input(ExecutableDir() / "asset_workspace.json");
            if (input)
            {
                try
                {
                    nlohmann::json binding;
                    input >> binding;
                    const std::filesystem::path source = binding.value("sourceRoot", "");
                    if (binding.value("version", 0) == 1 && std::filesystem::is_directory(source / "games"))
                        return source;
                }
                catch (const nlohmann::json::exception &)
                {
                }
            }
            return ExecutableDir();
        }
    } // namespace

    std::optional<std::string> ReadDefaultGame()
    {
        const std::filesystem::path path = ExecutableDir() / "engine.json";

        std::ifstream in(path);
        if (!in)
        {
            MIR_LOG_ERROR("no --game given and could not open '{}'", path.string());
            return std::nullopt;
        }

        nlohmann::json json;
        try
        {
            in >> json;
        }
        catch (const nlohmann::json::parse_error &e)
        {
            MIR_LOG_ERROR("'{}' is not valid JSON ({})", path.string(), e.what());
            return std::nullopt;
        }

        const auto it = json.find("defaultGame");
        if (it == json.end() || !it->is_string() || it->get<std::string>().empty())
        {
            MIR_LOG_ERROR("no --game given and '{}' names no defaultGame", path.string());
            return std::nullopt;
        }

        return it->get<std::string>();
    }

    std::optional<std::filesystem::path> NormalizeGameDir(const std::string &requested)
    {
        const std::filesystem::path rootDir = ProjectRoot();

        std::filesystem::path dir = std::filesystem::path(requested).lexically_normal();
        if (dir.is_absolute())
            dir = dir.lexically_relative(rootDir.lexically_normal());

        // lexically_normal keeps a trailing separator as an empty filename
        if (!dir.empty() && dir.filename().empty())
            dir = dir.parent_path();

        if (dir.empty() || dir == "." || dir.has_root_path() || *dir.begin() == "..")
        {
            MIR_LOG_ERROR("game directory '{}' is not inside '{}'", requested, rootDir.string());
            return std::nullopt;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(rootDir / dir, ec))
        {
            MIR_LOG_ERROR("game directory '{}' does not exist", (rootDir / dir).string());
            return std::nullopt;
        }

        return dir;
    }

    std::optional<GameProject> LoadGameProject(const std::filesystem::path &gameDir)
    {
        const std::filesystem::path root = ProjectRoot() / gameDir;
        const std::filesystem::path manifestPath = root / "game.json";

        std::ifstream in(manifestPath);
        if (!in)
        {
            MIR_LOG_ERROR("LoadGameProject: no game.json in '{}', not a game directory", root.string());
            return std::nullopt;
        }

        GameProject project;
        project.mDir = gameDir.generic_string();
        project.mName = gameDir.filename().string();

        // assetsRoot default must match CMakeLists.txt
        std::string assetsRoot = "assets";
        std::string sceneDir = "scenes/default";
        std::string soundBanksRoot = "assets/audio/SoundBanks";

        nlohmann::json json;
        try
        {
            in >> json;
        }
        catch (const nlohmann::json::parse_error &e)
        {
            MIR_LOG_ERROR("LoadGameProject: '{}' is not valid JSON ({}), using defaults", manifestPath.string(),
                          e.what());
        }

        if (json.is_object())
        {
            project.mName = json.value("name", project.mName);
            project.mTitle = json.value("title", project.mTitle);
            assetsRoot = json.value("assetsRoot", assetsRoot);
            sceneDir = json.value("sceneDir", sceneDir);
            soundBanksRoot = json.value("soundBanksRoot", soundBanksRoot);
        }

        if (project.mName.empty())
            project.mName = gameDir.filename().string();

        // normalized like AssetCooker's NormalizeRoot, so ids match
        std::string idRoot = (gameDir / assetsRoot).lexically_normal().generic_string();
        while (!idRoot.empty() && idRoot.back() == '/')
            idRoot.pop_back();

        project.mAssetIdRoot = std::move(idRoot);
        project.mSceneDir = root / sceneDir;
        project.mSoundBankDir = root / soundBanksRoot;
        project.mProjectSettingsPath = root / "project_settings.json";

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

        const std::filesystem::path gamesDir = ProjectRoot() / "games";

        std::error_code ec;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(gamesDir, ec))
        {
            std::error_code entryEc;
            if (!entry.is_directory(entryEc) || !std::filesystem::is_regular_file(entry.path() / "game.json", entryEc))
                continue;

            if (std::optional<GameProject> project =
                    LoadGameProject(std::filesystem::path("games") / entry.path().filename()))
                projects.push_back(std::move(*project));
        }

        if (ec)
            MIR_LOG_WARN("DiscoverGameProjects: could not list '{}': {}", gamesDir.string(), ec.message());

        std::sort(projects.begin(), projects.end(),
                  [](const GameProject &a, const GameProject &b) { return a.mDir < b.mDir; });
        return projects;
    }

    bool CreateGameProject(const std::string &name, bool useRayTracing)
    {
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "." || name == "..")
        {
            MIR_LOG_ERROR("CreateGameProject: '{}' is not a valid project name", name);
            return false;
        }

        const std::filesystem::path root = ProjectRoot() / "games" / name;

        std::error_code ec;
        if (std::filesystem::exists(root, ec))
        {
            MIR_LOG_ERROR("CreateGameProject: 'games/{}' already exists", name);
            return false;
        }

        const std::filesystem::path sceneDir = root / "scenes" / "default";
        if (!std::filesystem::create_directories(sceneDir, ec) ||
            !std::filesystem::create_directories(root / "assets", ec))
        {
            MIR_LOG_ERROR("CreateGameProject: could not create 'games/{}': {}", name, ec.message());
            return false;
        }

        const nlohmann::json manifest{{"name", name}, {"title", "MjolnirEngine - " + name}};
        const std::string manifestDump = manifest.dump(2);
        const std::span<const std::byte> manifestBytes(reinterpret_cast<const std::byte *>(manifestDump.data()),
                                                       manifestDump.size());

        // same shape as games/EmptyScene/scenes/default/scene.json
        const nlohmann::json scene{{"entities", nlohmann::json::array()}, {"name", "default"}, {"nextId", 1}};
        const std::string sceneDump = scene.dump(2);
        const std::span<const std::byte> sceneBytes(reinterpret_cast<const std::byte *>(sceneDump.data()),
                                                    sceneDump.size());

        if (!WriteFileBytes(root / "game.json", manifestBytes) || !WriteFileBytes(sceneDir / "scene.json", sceneBytes))
        {
            MIR_LOG_ERROR("CreateGameProject: could not write 'games/{}' manifest/scene", name);
            return false;
        }

        return RenderSettings{.rayTracing = useRayTracing}.SaveFile(root / "project_settings.json");
    }
} // namespace mir
