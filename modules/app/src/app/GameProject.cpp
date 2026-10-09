/**
 * @file GameProject.cpp
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameProject.h>

#include <assets/AssetPath.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/RenderSettings.h>
#include <scene/SceneIO.h>

#include <serial/JsonFile.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        // Where games/ lives. Normally that is next to the executable, beside
        // the copies CMake made. A development build also writes an
        // asset_workspace.json there pointing back at the source tree, so the
        // editor's import workflow edits the real game data instead of a copy.
        std::filesystem::path ProjectRoot()
        {
            const std::optional<nlohmann::json> binding =
                ReadJsonFile(ExecutableDir() / "asset_workspace.json");

            if (binding && binding->is_object() && binding->value("version", 0) == 1)
            {
                const std::filesystem::path source = binding->value("sourceRoot", "");
                std::error_code ec;
                if (std::filesystem::is_directory(source / "games", ec))
                    return source;
            }

            return ExecutableDir();
        }
    } // namespace

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
        std::string soundBanksRoot = "assets/audio/SoundBanks";

        // A malformed manifest keeps the defaults; ReadJsonFile logged why.
        if (const std::optional<nlohmann::json> json = ReadJsonFile(manifestPath); json && json->is_object())
        {
            project.mName = json->value("name", project.mName);
            project.mTitle = json->value("title", project.mTitle);
            assetsRoot = json->value("assetsRoot", assetsRoot);
            sceneDir = json->value("sceneDir", sceneDir);
            soundBanksRoot = json->value("soundBanksRoot", soundBanksRoot);
        }

        if (project.mName.empty())
            project.mName = gameDir.filename().string();

        // The same normalizer AssetCooker applies to its --source roots, so ids match
        AssetPathResult idRoot = MakeLogicalAssetPath(gameDir.generic_string(), assetsRoot);
        if (!idRoot.Ok())
        {
            MIR_LOG_ERROR("LoadGameProject: assetsRoot '{}' in '{}': {}", assetsRoot, manifestPath.string(),
                          Describe(idRoot.error));
            return std::nullopt;
        }

        project.mAssetIdRoot = std::move(idRoot.path);
        project.mRootDir = root;
        project.mSceneDir = root / sceneDir;
        project.mProjectSettingsPath = root / kProjectSettingsFileName;
        project.mSoundBankDir = root / soundBanksRoot;

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

        const nlohmann::json manifest{ { "name", name },{ "title", "MjolnirEngine - " + name } };

        // same shape as games/EmptyScene/scenes/default/scene.json
        const nlohmann::json scene{ { "entities", nlohmann::json::array() },{ "name", "default" },{ "nextId", 1 } };

        if (!WriteJsonFile(root / kGameManifestFileName, manifest) ||
            !WriteJsonFile(sceneDir / kSceneManifestFileName, scene))
        {
            MIR_LOG_ERROR("CreateGameProject: could not write 'games/{}' manifest/scene", name);
            return false;
        }

        return RenderSettings{ .rayTracing = useRayTracing }.SaveFile(root / kProjectSettingsFileName);
    }
}
