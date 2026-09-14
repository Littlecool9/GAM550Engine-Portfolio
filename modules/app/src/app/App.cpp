/**
 * @file App.cpp
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>

#include <core/EngineVersion.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/RenderSystem.h>
#include <scene/SceneIO.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptSystem.h>

#include <algorithm>
#include <chrono>

namespace mir
{
    App::~App()
    {
        Shutdown();
    }

    bool App::Initialize(const AppDesc &desc)
    {
        mDesc = desc;

        if (mDesc.mSceneDir.is_relative())
            mDesc.mSceneDir = ExecutableDir() / mDesc.mSceneDir;
        if (mDesc.mProjectSettingsPath.is_relative())
            mDesc.mProjectSettingsPath = ExecutableDir() / mDesc.mProjectSettingsPath;

        EnsureDpiAware();

        if (!mSplash.Show({.mEngineName = desc.mAppName,
                           .mVersion = kEngineVersion,
                           .mCopyright = kEngineCopyright,
                           .mStatus = "Creating window...",
                           .mProgress = 0.0f}))
        {
            MIR_LOG_WARN("Splash screen failed to show, continuing without it");
        }

        WindowDesc windowDesc{};
        windowDesc.mWidth = desc.mWidth;
        windowDesc.mHeight = desc.mHeight;
        windowDesc.mTitle = desc.mTitle;
        windowDesc.mMaximized = true;
        windowDesc.mCustomTitleBar = desc.mEnableEditorLayout;
        windowDesc.mStartHidden = true;

        mWindow = Window::Create(windowDesc);
        if (!mWindow)
        {
            MIR_LOG_ERROR("Window creation failed");
            return false;
        }

        mSplash.SetProgress("Initializing renderer...", 0.2f);
        if (!mRenderer.Initialize({.window = mWindow.get(),
                                   .appName = desc.mAppName,
                                   .enableValidation = desc.mEnableValidation}))
        {
            MIR_LOG_ERROR("Renderer initialization failed");
            mWindow.reset();
            return false;
        }

        mSplash.SetProgress("Initializing editor...", 0.5f);
        if (!mEditor.Initialize(*mWindow, mRenderer, mDesc.mProjectSettingsPath))
        {
            MIR_LOG_ERROR("Editor initialization failed");
            mRenderer.Shutdown();
            mWindow.reset();
            return false;
        }

        mInitialized = true;

        mSplash.SetProgress("Registering components...", 0.75f);

        // scene graph + install destroy hook
        InstallHierarchy(mWorld);

        // Before anything may load a script
        RegisterCoreComponents();
        RegisterRendererComponents();
        RegisterScriptComponents();

        // defers structural change
        mWorld.EmplaceResource<FrameCommands>(FrameCommands{&mCommands});

        mWorld.EmplaceResource<InputMap>(InputMap::LoadFile(mDesc.mProjectSettingsPath));
        mWorld.EmplaceResource<InputState>();

        mScheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, *mWindow);

        mScheduler.AddSystem<ScriptSystem>(SystemPhase::PreUpdate, mScriptHost);

        // should be before any other system in PostUpdate
        mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);

        mScheduler.AddSystem<RenderSystem>(SystemPhase::Render, mRenderer);

        mSplash.SetProgress("Loading scene...", 0.9f);
        mScene = mir::NewScene("untitled");

        mWindow->Show();

        return true;
    }

    void App::NewScene(std::string name)
    {
        UnloadScene(mWorld, mScene);
        mScene = mir::NewScene(std::move(name));
    }

    bool App::SaveScene()
    {
        return mir::SaveScene(mWorld, mDesc.mSceneDir, mScene);
    }

    bool App::LoadScene()
    {
        // scene hardcoded at the moment
        if (!std::filesystem::exists(mDesc.mSceneDir / "scene.json"))
        {
            MIR_LOG_ERROR("App::LoadScene: no scene.json in '{}'", mDesc.mSceneDir.string());
            return false;
        }

        UnloadScene(mWorld, mScene);
        mScene = mir::LoadScene(mWorld, mDesc.mSceneDir);
        return true;
    }

    AssetCache *App::Assets()
    {
        if (mAssetCache.has_value())
            return &*mAssetCache;

        if (mAssetLoadFailed)
            return nullptr; // already tried and logged; do not re-stat the disk every call

        const std::filesystem::path manifestPath = CookedAssetsDir() / "manifest.blob";
        mAssetManifest = AssetManifest::LoadFile(manifestPath);
        if (!mAssetManifest.has_value())
        {
            MIR_LOG_ERROR("Asset manifest load failed, assets unavailable: {}", manifestPath.string());
            mAssetLoadFailed = true;
            return nullptr;
        }

        // after the manifest is engaged, never before: the cache stores a raw
        // pointer to it
        mAssetCache.emplace(&*mAssetManifest, CookedAssetsDir());
        return &*mAssetCache;
    }

    SystemContext App::MakeContext(float dt)
    {
        return SystemContext{mWorld, mCommands, dt, mElapsed, mFrame};
    }

    void App::Run()
    {
        if (!mInitialized)
        {
            return;
        }

        SystemContext startContext = MakeContext(0.0f);
        mScheduler.Start(startContext);

        auto previous = std::chrono::steady_clock::now();

        while (!mWindow->ShouldClose())
        {
            mWindow->PollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(),
                                      mDesc.mMaxDeltaSeconds);
            previous = now;
            mElapsed += dt;

            mEditor.BeginFrame();

            if (mWindow->Width() != 0 && mWindow->Height() != 0)
            {
                switch (mEditor.DrawLayout(mDesc.mEnableEditorLayout, mWorld.Resource<InputMap>()))
                {
                case SceneMenuAction::New:
                    NewScene();
                    break;
                case SceneMenuAction::Save:
                    SaveScene();
                    break;
                case SceneMenuAction::Load:
                    LoadScene();
                    break;
                case SceneMenuAction::None:
                    break;
                }
            }

            mRenderer.SetImGuiDrawData(mEditor.EndFrame());
            mRenderer.SetSceneViewport(mEditor.SceneViewportRect());

            mWorld.Resource<InputState>().SetUiCapture(mEditor.WantsCaptureKeyboard(), mEditor.WantsCaptureMouse());

            mScriptReloadWatcher.Poll(Assets(), dt);

            SystemContext context = MakeContext(dt);
            mScheduler.Update(context);

            ++mFrame;

            // same path as the close button, so the loop and Shutdown see an ordinary exit
            if (mDesc.mMaxFrames != 0 && mFrame >= mDesc.mMaxFrames)
                mWindow->RequestClose();
        }
    }

    void App::Shutdown()
    {
        if (!mInitialized)
        {
            return;
        }

        // let all the systems stop first
        SystemContext stopContext = MakeContext(0.0f);
        mScheduler.Stop(stopContext);

        mScheduler.Reset();

        // Cache before manifest
        mAssetCache.reset();
        mAssetManifest.reset();
        mAssetLoadFailed = false;

        mEditor.Shutdown(mRenderer);

        mRenderer.Shutdown();
        mWindow.reset();
        mInitialized = false;
        MIR_LOG_INFO("App shut down");
    }
}
