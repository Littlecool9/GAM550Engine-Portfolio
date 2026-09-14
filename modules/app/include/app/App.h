/**
 * @file App.h
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace mir
{
    struct AppDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;
        const char *mTitle = "MjolnirEngine";
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;

        bool mEnableEditorLayout = true;

        float mMaxDeltaSeconds = 0.25f;

        // Run requests a close after this many frames; 0 runs until the window closes
        uint64_t mMaxFrames = 0;

        // temp default path for scenes
        std::filesystem::path mSceneDir = "scenes/default";

        // Where the Project Settings window's Save button writes
        std::filesystem::path mProjectSettingsPath = "project_settings.json";
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

        // Discards mScene's entities and replaces it with an empty one
        void NewScene(std::string name = "untitled");

        // Writes Scene() to mSceneDir. False on I/O failure
        bool SaveScene();

        // Discards mScene's entities and replaces it with mSceneDir's
        // contents. False if mSceneDir has no scene.json to load.
        bool LoadScene();

    private:
        // context is rebuilt per tick
        SystemContext MakeContext(float dt);

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
        double mElapsed = 0.0;
        uint64_t mFrame = 0;

        // maintain order of mAssetCache -> mAssetCache
        std::optional<AssetManifest> mAssetManifest;
        std::optional<AssetCache> mAssetCache;
        bool mAssetLoadFailed = false;
        bool mInitialized = false;
    };
}
