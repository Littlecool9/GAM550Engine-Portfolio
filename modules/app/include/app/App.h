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
#include <scene/PlaySnapshot.h>
#include <scene/SceneAsset.h>
#include <script/ScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <window/Splash.h>
#include <window/Window.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    class RenderSystem;
    class ScriptSystem;

    struct AppDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;

        // Vulkan instance name. Engine-level, not project data
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;

        bool mEnableEditorLayout = true;

        float mMaxDeltaSeconds = 0.25f;

        // Path to the project's game.json, relative to the executable
        std::filesystem::path mProjectPath;

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

        bool LoadScene();

        bool LoadScriptAsset(std::string_view assetPath, std::string_view scriptName);

        // Edit -> Play
        void PlayScene();

        // Play -> Edit
        void StopScene();

        // Toggles Running/Paused. No-op outside Play.
        void TogglePause();

    private:
        // context is rebuilt per tick
        SystemContext MakeContext(float dt);

        void ResolveSceneMeshes();
        void ResolveSceneScripts();

        SplashScreen mSplash;
        std::unique_ptr<Window> mWindow;
        VulkanRenderer mRenderer;
        Editor mEditor;

        // Before mWorld, so it is destroyed after it
        CommandBuffer mCommands;

        World mWorld;
        SystemScheduler mScheduler;
        RenderSystem *mRenderSystem = nullptr;
        ScriptSystem *mScriptSystem = nullptr;
        PlaySnapshot mPlaySnapshot;
        ScriptHost mScriptHost;
        ScriptReloadWatcher mScriptReloadWatcher{ mScriptHost };
        LoadedScene mScene;

        AppDesc mDesc;

        std::filesystem::path mSceneDir;
        std::string mAssetsRoot;

        double mElapsed = 0.0;
        uint64_t mFrame = 0;

        // maintain order of mAssetCache -> mAssetCache
        std::optional<AssetManifest> mAssetManifest;
        std::optional<AssetCache> mAssetCache;
        bool mAssetLoadFailed = false;
        bool mInitialized = false;
    };
}
