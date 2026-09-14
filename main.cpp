#include <app/App.h>
#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <renderer/Shapes.h>
#include <renderer/components/MeshRenderer.h>
#include <script/components/ScriptRef.h>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
    /// What the command line asked for. Both are optional: no --game loads
    /// engine.json's defaultGame, no --frames runs until the window closes.
    struct LaunchOptions
    {
        std::string mGameDir;
        uint64_t mMaxFrames = 0;
    };

    /// `[--game <dir>] [--frames <N>]`. Anything else is logged and fails,
    /// so a mistyped flag can't quietly fall through to the default game.
    std::optional<LaunchOptions> ParseLaunchOptions(int argc, char **argv)
    {
        LaunchOptions options;

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--game" && i + 1 < argc)
            {
                options.mGameDir = argv[++i];
            }
            else if (arg == "--frames" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                const auto [parsedEnd, ec] = std::from_chars(value.data(), value.data() + value.size(), options.mMaxFrames);
                if (ec != std::errc{} || parsedEnd != value.data() + value.size() || options.mMaxFrames == 0)
                {
                    MIR_LOG_ERROR("--frames expects a positive integer, got '{}'", value);
                    return std::nullopt;
                }
            }
            else
            {
                MIR_LOG_ERROR("unrecognized argument '{}' (usage: [--game <dir>] [--frames <N>])", arg);
                return std::nullopt;
            }
        }

        return options;
    }

    /// engine.json's defaultGame, read from next to the executable - the
    /// game to load when --game isn't given.
    std::optional<std::string> ReadDefaultGame()
    {
        const std::filesystem::path path = mir::ExecutableDir() / "engine.json";

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

    /// Brings --game or defaultGame into the one form asset ids can be built
    /// from. The cooker hashed "games/<name>/assets/...", so "games\<name>\",
    /// "./games/<name>" and an absolute path into the run directory all have
    /// to come out as "games/<name>". Nullopt (logged) for a directory outside
    /// the run directory or not there at all.
    std::optional<std::filesystem::path> NormalizeGameDir(const std::string &requested)
    {
        const std::filesystem::path &exeDir = mir::ExecutableDir();

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

    /// Everything main.cpp derives from a game directory. The paths inside
    /// game.json are relative to that directory, so a game can be renamed or
    /// copied without editing it.
    struct GameProject
    {
        std::string mTitle = "MjolnirEngine";

        // "<gameDir>/<assetsRoot>" exactly as the cooker hashed it into this
        // game's asset ids, e.g. "games/<name>/assets" - not a disk path
        std::string mAssetIdRoot;

        std::filesystem::path mSceneDir;
        std::filesystem::path mProjectSettingsPath;
    };

    /// Nullopt when `gameDir` has no game.json: that file is what makes a
    /// directory a game (CMakeLists.txt discovers games the same way), and a
    /// wrong --game should stop here rather than open an empty scene. A
    /// game.json that fails to parse still loads, on the defaults below, with
    /// a logged reason - a content problem, not a reason not to start.
    std::optional<GameProject> LoadGameProject(const std::filesystem::path &gameDir)
    {
        const std::filesystem::path root = mir::ExecutableDir() / gameDir;
        const std::filesystem::path manifestPath = root / "game.json";

        std::ifstream in(manifestPath);
        if (!in)
        {
            MIR_LOG_ERROR("LoadGameProject: no game.json in '{}', not a game directory", root.string());
            return std::nullopt;
        }

        GameProject project;

        // This default is repeated in CMakeLists.txt, which cooks the same root
        std::string assetsRoot = "assets";
        std::string sceneDir = "scenes/default";

        nlohmann::json json;
        try
        {
            in >> json;
        }
        catch (const nlohmann::json::parse_error &e)
        {
            MIR_LOG_ERROR("LoadGameProject: '{}' is not valid JSON ({}), using defaults", manifestPath.string(), e.what());
        }

        if (json.is_object())
        {
            project.mTitle = json.value("title", project.mTitle);
            assetsRoot = json.value("assetsRoot", assetsRoot);
            sceneDir = json.value("sceneDir", sceneDir);
        }

        // Normalized the way the cooker's NormalizeRoot is, so "assets/" and
        // "./assets" produce the same ids as "assets"
        std::string idRoot = (gameDir / assetsRoot).lexically_normal().generic_string();
        while (!idRoot.empty() && idRoot.back() == '/')
            idRoot.pop_back();

        project.mAssetIdRoot = std::move(idRoot);
        project.mSceneDir = root / sceneDir;
        project.mProjectSettingsPath = root / "project_settings.json";
        return project;
    }

    /// --game if given, else engine.json's defaultGame. Every failure on the
    /// way is logged where it happens.
    std::optional<GameProject> ResolveGameProject(const LaunchOptions &options)
    {
        std::optional<std::string> requested = options.mGameDir;
        if (options.mGameDir.empty())
            requested = ReadDefaultGame();
        if (!requested)
            return std::nullopt;

        const std::optional<std::filesystem::path> gameDir = NormalizeGameDir(*requested);
        if (!gameDir)
            return std::nullopt;

        MIR_LOG_INFO("Loading game '{}'", gameDir->generic_string());
        return LoadGameProject(*gameDir);
    }

    /// Loads a `.lua` asset by its cooked path and registers it with the
    /// script host under `scriptName`, then hands it to the reload watcher
    /// so an edit followed by a rebuild is picked up without restarting.
    /// False when assets aren't available or the file failed to load or
    /// parse - logged by AssetCache/ScriptHost already, so the caller just
    /// decides whether to attach the script.
    bool LoadScriptAsset(mir::App &app, std::string_view assetPath, std::string_view scriptName)
    {
        mir::AssetCache *cache = app.Assets();
        if (cache == nullptr)
            return false;

        const mir::AssetId id = mir::MakeAssetId(assetPath);
        const mir::AssetBlobView *blob = cache->Load(id);
        if (blob == nullptr)
            return false;

        if (!app.Scripts().LoadScriptSource(scriptName, mir::AsStringView(*blob)))
            return false;

        app.ScriptReload().Track(std::string(scriptName), id);
        return true;
    }

    /// Runs after a scene loads from disk: every ScriptRef LoadScene added has
    /// scriptName set (that's the field SceneIO round-trips) but instanceRef
    /// still at -1 (its default, deliberately not saved - see ScriptRef.h),
    /// because "spawn a live Lua instance" is a side-effecting call SceneIO's
    /// generic field-copy loop has no hook for. This is that hook, run once
    /// up here instead: for every such entity, load <assetsRoot>/scripts/
    /// <name>.lua by convention and wire up the instance ScriptSystem will
    /// then drive.
    void ResolveSceneScripts(mir::App &app, const GameProject &project)
    {
        mir::World &world = app.GetWorld();

        world.GetOrCreateQuery<mir::ScriptRef>().ForEach(
            [&](mir::Entity, mir::ScriptRef &ref)
            {
                if (ref.instanceRef >= 0 || ref.scriptName[0] == '\0')
                    return;

                const std::string name = ref.scriptName;
                const std::string assetPath = project.mAssetIdRoot + "/scripts/" + name + ".lua";

                if (!LoadScriptAsset(app, assetPath, name))
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: could not load '{}' for script '{}'", assetPath, name);
                    return;
                }

                ref.instanceRef = app.Scripts().CreateInstance(name);
            });
    }

    /// A mesh asset is just MeshData (Shapes.h) as JSON - vertices (position/
    /// color/normal) and indices, the same shape CreateMesh already takes.
    /// No importer, no binary format: hand- or tool-authored JSON is enough
    /// until there's a reason for more. Malformed JSON logs and returns
    /// nullopt rather than throwing - one bad mesh asset shouldn't take the
    /// whole load down.
    std::optional<mir::MeshData> ParseMeshAsset(std::string_view text, std::string_view assetPath)
    {
        nlohmann::json json;
        try
        {
            json = nlohmann::json::parse(text);
        }
        catch (const nlohmann::json::parse_error &e)
        {
            MIR_LOG_ERROR("ParseMeshAsset: '{}' is not valid JSON ({})", assetPath, e.what());
            return std::nullopt;
        }

        mir::MeshData mesh;
        for (const auto &v : json.value("vertices", nlohmann::json::array()))
        {
            mir::Vertex vertex{};
            const auto &p = v.at("position");
            const auto &c = v.at("color");
            const auto &n = v.at("normal");
            vertex.pos = glm::vec3(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
            vertex.color = glm::vec3(c.at(0).get<float>(), c.at(1).get<float>(), c.at(2).get<float>());
            vertex.normal = glm::vec3(n.at(0).get<float>(), n.at(1).get<float>(), n.at(2).get<float>());
            mesh.vertices.push_back(vertex);
        }
        mesh.indices = json.value("indices", std::vector<uint32_t>{});
        return mesh;
    }

    /// Runs after a scene loads from disk, same shape as ResolveSceneScripts:
    /// meshName/materialShader are what SceneIO round-trips (see
    /// MeshRenderer.h), mesh/material are this run's live GPU handles, still
    /// null until this pass fills them in. Handles are cached by asset id /
    /// shader name so entities sharing one mesh or material asset share one
    /// upload instead of duplicating it per entity.
    void ResolveSceneMeshes(mir::App &app, const GameProject &project)
    {
        mir::World &world = app.GetWorld();
        mir::AssetCache *cache = app.Assets();

        std::unordered_map<uint64_t, mir::MeshHandle> meshCache;
        std::unordered_map<std::string, mir::MaterialHandle> materialCache;

        world.GetOrCreateQuery<mir::MeshRenderer>().ForEach(
            [&](mir::Entity, mir::MeshRenderer &renderer)
            {
                if (renderer.mesh.IsNull() && renderer.meshName[0] != '\0')
                {
                    const std::string assetPath = project.mAssetIdRoot + "/" + renderer.meshName;
                    const mir::AssetId id = mir::MakeAssetId(assetPath);

                    if (auto it = meshCache.find(id.value); it != meshCache.end())
                    {
                        renderer.mesh = it->second;
                    }
                    else if (cache != nullptr)
                    {
                        if (const mir::AssetBlobView *blob = cache->Load(id); blob != nullptr)
                        {
                            if (auto mesh = ParseMeshAsset(mir::AsStringView(*blob), assetPath))
                            {
                                renderer.mesh = app.Renderer().CreateMesh(mesh->vertices, mesh->indices);
                                meshCache.emplace(id.value, renderer.mesh);
                            }
                        }
                        else
                        {
                            MIR_LOG_ERROR("ResolveSceneMeshes: could not load '{}'", assetPath);
                        }
                    }
                }

                if (renderer.material.IsNull() && renderer.materialShader[0] != '\0')
                {
                    const std::string shaderName = renderer.materialShader;
                    if (auto it = materialCache.find(shaderName); it != materialCache.end())
                        renderer.material = it->second;
                    else
                        renderer.material = materialCache
                                                 .emplace(shaderName, app.Renderer().CreateMaterial(
                                                                          mir::MaterialDesc{.shaderName = shaderName}))
                                                 .first->second;
                }
            });
    }

    /// Whether the log history (the buffer behind the editor's Output panel)
    /// holds an error. It is a bounded ring, far larger than a smoke run logs.
    bool AnyErrorLogged()
    {
        for (const mir::LogEntry &entry : mir::LogHistory::Snapshot())
        {
            if (entry.level == mir::LogLevel::Error || entry.level == mir::LogLevel::Critical)
                return true;
        }
        return false;
    }
}

int main(int argc, char **argv)
{
    // Logging lives outside App so early construction failures are still visible.
    mir::InitLog();

    // Resolved before App exists: a bad argument or game directory exits
    // without ever showing the splash.
    const std::optional<LaunchOptions> options = ParseLaunchOptions(argc, argv);
    const std::optional<GameProject> project = options ? ResolveGameProject(*options) : std::nullopt;
    if (!project)
    {
        mir::FlushLog();
        return 1;
    }

    mir::App app;

    mir::AppDesc desc{};
    desc.mTitle = project->mTitle.c_str();
    desc.mSceneDir = project->mSceneDir;
    desc.mProjectSettingsPath = project->mProjectSettingsPath;
    desc.mMaxFrames = options->mMaxFrames;

    if (!app.Initialize(desc))
    {
        mir::FlushLog();
        return -1;
    }

    // A game with no saved scene yet keeps the empty scene Initialize made.
    // Checked here rather than left to LoadScene, which logs a missing
    // scene.json as an error because from the File menu it is one.
    std::error_code ec;
    if (!std::filesystem::exists(project->mSceneDir / "scene.json", ec))
    {
        MIR_LOG_INFO("No scene.json in '{}', starting with an empty scene", project->mSceneDir.string());
    }
    else if (app.LoadScene())
    {
        app.Splash().SetProgress("Resolving scripts...", 0.93f);
        ResolveSceneScripts(app, *project);

        app.Splash().SetProgress("Resolving meshes...", 0.97f);
        ResolveSceneMeshes(app, *project);
    }

    // Only now is startup actually done - close explicitly rather than
    // leaning on WS_EX_TOPMOST z-order to hide the swap.
    app.Splash().SetProgress("Ready", 1.0f);
    app.Splash().Close();

    app.Run();
    app.Shutdown();

    // A bounded run is how the smoke tests drive this, and an error in the
    // log fails one as surely as a crash. Unbounded runs exit 0 as before.
    const int exitCode = (options->mMaxFrames != 0 && AnyErrorLogged()) ? 1 : 0;

    mir::FlushLog();
    return exitCode;
}
