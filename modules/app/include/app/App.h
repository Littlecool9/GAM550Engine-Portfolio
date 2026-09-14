/**
 * @file App.h
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <app/GameProject.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <editor/Editor.h>
#include <renderer/VulkanRenderer.h>
#include <scene/SceneAsset.h>
#include <script/ScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <window/Splash.h>
#include <window/Window.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    struct AppDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;

        bool mEnableEditorLayout = true;

        float mMaxDeltaSeconds = 0.25f;

        // Run requests a close after this many frames; 0 runs until the window closes
        uint64_t mMaxFrames = 0;

        // The game Initialize opens: window title, project settings and scene
        GameProject mProject;

        // Test hook for bounded runs: game directories opened one after another,
        // spread evenly across mMaxFrames, through the same OpenProject the
        // editor's Project > Open Project menu uses
        std::vector<std::string> mProjectSwitches;
    };

    class App
    {
    public:
        App() = default;
        ~App();

        App(const App &) = delete;
        App &operator=(const App &) = delete;
        App(App &&) = delete;
        App &operator=(App &&) = delete;

        bool Initialize(const AppDesc &desc);
        void Run();
        void Shutdown();

        World &GetWorld() { return mWorld; }
        SystemScheduler &Systems() { return mScheduler; }
        ScriptHost &Scripts() { return mScriptHost; }
        ScriptReloadWatcher &ScriptReload() { return mScriptReloadWatcher; }

        // Temporary seam
        VulkanRenderer &Renderer() { return mRenderer; }

        // Startup splash
        SplashScreen &Splash() { return mSplash; }

        // The asset cache, loading the manifest on first use
        AssetCache *Assets();

        // The scene currently in the World
        LoadedScene &Scene() { return mScene; }

        // The game currently open
        const GameProject &Project() const { return mProject; }

        // Every game found next to the executable at startup
        const std::vector<GameProject> &AvailableProjects() const { return mAvailableProjects; }

        // Closes the open game and opens `requestedDir` (e.g. "games/EmptyScene"):
        // its scene, scripts, GPU resources, input map and window title all
        // switch over. False (logged) when the directory is not a game, in
        // which case the open game is left exactly as it was.
        bool OpenProject(const std::string &requestedDir);

        // Discards mScene's entities and replaces it with an empty one
        void NewScene(std::string name = "untitled");

        // Writes Scene() to the open game's scene directory. False on I/O failure
        bool SaveScene();

        // Discards mScene's entities and replaces them with the open game's
        // saved scene, resolving its script and mesh names into live instances
        // and GPU handles. False if the game has no scene.json to load.
        bool LoadScene();

    private:
        // context is rebuilt per tick
        SystemContext MakeContext(float dt);

        // Unloads mScene - destroy hooks drop each entity's Lua instance - then
        // frees the GPU meshes and materials nothing references any more
        void UnloadCurrentScene();

        // LoadScene when the open game has saved a scene; a new game without
        // one keeps the empty scene, which is not an error
        void LoadProjectScene();

        // Opens the next of mDesc.mProjectSwitches once its frame has come
        void RunProjectSwitches();

        SplashScreen mSplash;
        std::unique_ptr<Window> mWindow;
        VulkanRenderer mRenderer;
        Editor mEditor;

        // Before mWorld, so it is destroyed after it
        CommandBuffer mCommands;

        World mWorld;
        SystemScheduler mScheduler;
        ScriptHost mScriptHost;
        ScriptReloadWatcher mScriptReloadWatcher{mScriptHost};
        LoadedScene mScene;

        AppDesc mDesc;
        GameProject mProject;
        std::vector<GameProject> mAvailableProjects;
        std::vector<ProjectMenuEntry> mProjectMenu;
        std::size_t mNextProjectSwitch = 0;
        double mElapsed = 0.0;
        uint64_t mFrame = 0;

        // maintain order of mAssetCache -> mAssetCache
        std::optional<AssetManifest> mAssetManifest;
        std::optional<AssetCache> mAssetCache;
        bool mAssetLoadFailed = false;
        bool mInitialized = false;
    };
}
