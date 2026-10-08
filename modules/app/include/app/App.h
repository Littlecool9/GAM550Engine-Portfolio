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
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/FixedTimestep.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <gameshell/GameShell.h>
#include <gameui/UIContext.h>
#include <gameui/UIDemoScreen.h>
#include <imguiruntime/ImGuiRuntime.h>
#include <renderer/VulkanRenderer.h>
#include <scene/PlaySnapshot.h>
#include <scene/SceneAsset.h>
#include <script/ScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <window/Splash.h>
#include <window/Window.h>

// 1 with the editor, 0 for the game alone
#ifndef MIR_WITH_EDITOR 
#error "MIR_WITH_EDITOR is set by linking mir::app"
#endif

#if MIR_WITH_EDITOR
#include <app/AssetImportManager.h>
#include <editor/Editor.h>
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    class RenderSystem;
    class ScriptSystem;
    class ModelSystem;
    class TerrainRuntime;

    struct AppDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;

        // Vulkan instance name. Engine-level, not project data
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;

        bool mEnableEditorLayout = true;

        float mMaxDeltaSeconds = 0.25f;

        // FixedUpdate: seconds per substep, and at most this many substeps a frame
        float mFixedDeltaSeconds = 1.0f / 60.0f;
        uint32_t mMaxFixedSteps = 4;

        // 0: run until the window closes
        uint64_t mMaxFrames = 0;
        std::filesystem::path mCapturePath;

        // debug: draw UIDemoScreen, every core widget, as the game UI
        bool mUIDemo = false;

        // the game Initialize opens
        GameProject mProject;

        // test hook: RequestOpenProject this halfway through mMaxFrames; empty for
        // none
        std::string mSwitchProjectTo;
    };

    class App
    {
    public:
        //whether this build has the editor in it at all
        static constexpr bool kHasEditor = MIR_WITH_EDITOR != 0;

        App();
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

        // Games found at startup
        const std::vector<GameProject> &AvailableProjects() const { return mAvailableProjects; }

        // Validates the game and closes the window; main.cpp then relaunches into it.
        // False (logged) if it is not a game.
        bool RequestOpenProject(const std::string &requestedDir);

        // The game RequestOpenProject accepted, if any
        const std::optional<GameProject> &ProjectToOpen() const { return mProjectToOpen; }

        // Discards mScene's entities and replaces it with an empty one
        void NewScene(std::string name = "untitled");

        // Writes Scene() to the game's scene directory. False on I/O failure
        bool SaveScene();

        // Replaces Scene() with the game's saved scene and resolves it. False without
        // a scene.json
        bool LoadScene();

        // Edit -> Play
        void PlayScene();

        // Play -> Edit
        void StopScene();

        // Toggles Running/Paused. No-op outside Play.
        void TogglePause();

        //carries out what the player asked of the game in the pause menu
        void ApplyShellAction(GameShellAction action);

        //windowed to fullscreen without a restart. called by f11.
        //swapchain is recreated next frame.
        void ToggleFullscreen();

    private:
        // context is rebuilt per tick
        SystemContext MakeContext(float dt);

        // UnloadScene, then free the GPU resources
        void UnloadCurrentScene();

        // LoadScene if the game has a scene.json
        void LoadProjectScene();

        // the mSwitchProjectTo test hook
        void RunSwitchHook();

        //the game UI's area: the editor's scene viewport, or the whole window
        UIRect GameUIArea() const;

#if MIR_WITH_EDITOR
        //what the editor's menus, toolbar and panels asked for this frame
        void ApplyEditorActions(const EditorActions &actions);
        void ApplyImportedAssets();
#endif

        SplashScreen mSplash;
        std::unique_ptr<Window> mWindow;
        VulkanRenderer mRenderer;
        ImGuiRuntime mImGui;
#if MIR_WITH_EDITOR
        Editor mEditor;
#endif
        UIContext mGameUI;
        UIDemoScreen mUIDemo;
        GameShell mGameShell;
#if MIR_WITH_EDITOR
        AssetImportManager mAssetImport;
#endif

        // Before mWorld, so it is destroyed after it
        CommandBuffer mCommands;

        World mWorld;
        SystemScheduler mScheduler;
        RenderSystem *mRenderSystem = nullptr;
        ScriptSystem *mScriptSystem = nullptr;
        ModelSystem *mModelSystem = nullptr;
        PlaySnapshot mPlaySnapshot;
        ScriptHost mScriptHost;
        ScriptReloadWatcher mScriptReloadWatcher{mScriptHost};
        LoadedScene mScene;

        // Handles only, freed with the renderer and the World
        std::unique_ptr<TerrainRuntime> mTerrain;

        AppDesc mDesc;
        GameProject mProject;
        std::vector<GameProject> mAvailableProjects;
#if MIR_WITH_EDITOR
        std::vector<ProjectMenuEntry> mProjectMenu;
#endif
        std::optional<GameProject> mProjectToOpen;
        bool mSwitchHookFired = false;
        double mElapsed = 0.0;
        uint64_t mFrame = 0;
        FixedTimestep mFixedTimestep;

        // maintain order of mAssetCache -> mAssetCache
        std::optional<AssetManifest> mAssetManifest;
        std::optional<AssetCache> mAssetCache;
        bool mAssetLoadFailed = false;
        bool mInitialized = false;
    };
} // namespace mir
