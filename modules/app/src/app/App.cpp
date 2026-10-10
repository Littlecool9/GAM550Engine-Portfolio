/**
 * @file App.cpp
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>

#include "MeshResolveSystem.h"
#include "SceneResolve.h"
#include "TerrainRuntime.h"
#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <assets/AssetPath.h>
#include <audio/AudioEmitterSystem.h>
#include <audio/AudioListenerSystem.h>
#include <audio/AudioSystem.h>
#include <audio/ComponentRegistration.h>
#include <core/EngineVersion.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneIO.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptSystem.h>
#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainScene.h>

#if MIR_WITH_EDITOR
#include <editor/EntityBaseline.h>
#include <editor/Selection.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
    #if MIR_WITH_EDITOR
        UIRect ToUIRect(const VkRect2D &rect)
        {
            return UIRect{ static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                          static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height) };
        }
    #endif

        UIPointer ToUIPointer(const RawInputSnapshot &input)
        {
            return UIPointer{ static_cast<float>(input.mouseX), static_cast<float>(input.mouseY),
                             input.IsDown(MouseButton::Left) };
        }
    }

    // Never null; does nothing until Initialize binds it.
    App::App() : mTerrain(std::make_unique<TerrainRuntime>()) {}

    App::~App() { Shutdown(); }

    bool App::Initialize(const AppDesc &desc)
    {
        mDesc = desc;
        mProject = desc.mProject;
        mProjectToOpen.reset();
        mSwitchHookFired = false;
        mFixedTimestep = FixedTimestep{ .mStep = desc.mFixedDeltaSeconds, .mMaxSteps = desc.mMaxFixedSteps };

        //audio teardown to call when other things fail to initialize
        const auto fail = [this]
            {
            #if MIR_WITH_EDITOR
                mAssetImport.Shutdown();
            #endif
                audio::Terminate();
                return false;
            };

    #if MIR_WITH_EDITOR
        mAssetImport.Initialize(mProject);
    #else
            //no editor to lay out, the window is the game
        mDesc.mEnableEditorLayout = false;
        MIR_LOG_INFO("Built without the editor; running the game");
    #endif

        audio::Init(mProject.mSoundBankDir);

        EnsureDpiAware();

        if (!mSplash.Show({ .mEngineName = desc.mAppName,
                           .mVersion = kEngineVersion,
                           .mCopyright = kEngineCopyright,
                           .mStatus = "Creating window...",
                           .mProgress = 0.0f }))
        {
            MIR_LOG_WARN("Splash screen failed to show, continuing without it");
        }

        WindowDesc windowDesc{};
        windowDesc.mWidth = desc.mWidth;
        windowDesc.mHeight = desc.mHeight;
        windowDesc.mTitle = mProject.mTitle.c_str();
        windowDesc.mMaximized = mDesc.mEnableEditorLayout;
        windowDesc.mCustomTitleBar = mDesc.mEnableEditorLayout;
        windowDesc.mStartHidden = true;

        mWindow = Window::Create(windowDesc);
        if (!mWindow)
        {
            MIR_LOG_ERROR("Window creation failed");
            return fail();
        }

        mSplash.SetProgress("Initializing renderer...", 0.2f);
        if (!mRenderer.Initialize(
            { .window = mWindow.get(), .appName = desc.mAppName, .enableValidation = desc.mEnableValidation }))
        {
            MIR_LOG_ERROR("Renderer initialization failed");
            // Frees whatever Initialize created before it failed; the window must outlive the surface.
            mRenderer.Shutdown();
            mWindow.reset();
            return fail();
        }

        mSplash.SetProgress("Initializing UI...", 0.4f);
        if (!mImGui.Initialize(*mWindow, mRenderer))
        {
            MIR_LOG_ERROR("ImGui initialization failed");
            mRenderer.Shutdown();
            mWindow.reset();
            return fail();
        }

    #if MIR_WITH_EDITOR
        mSplash.SetProgress("Initializing editor...", 0.5f);
        if (!mEditor.Initialize(mImGui, *mWindow, mProject.mProjectSettingsPath, mWorld))
        {
            MIR_LOG_ERROR("Editor initialization failed");
            mImGui.Shutdown(mRenderer);
            mRenderer.Shutdown();
            mWindow.reset();
            return fail();
        }
    #endif

        mInitialized = true;

        mSplash.SetProgress("Registering components...", 0.75f);

        // scene graph + install destroy hook
        InstallHierarchy(mWorld);

        // Before anything may load a script
        RegisterCoreComponents();
        RegisterRendererComponents();
        RegisterRendererEntityTypes();
        RegisterPhysicsComponents();
        RegisterScriptComponents();
        RegisterAudioComponents();
        RegisterModelPlayer();
        RegisterTerrainComponents();

        mWorld.EmplaceResource<ModelDrawItems>();
        mWorld.EmplaceResource<ModelMorphOverrides>();
        mWorld.EmplaceResource<TerrainDrawItems>();
        mWorld.EmplaceResource<TerrainWorld>();

        // defers structural change
        mWorld.EmplaceResource<FrameCommands>(FrameCommands{ &mCommands });

        InputMap inputMap = InputMap::LoadFile(mProject.mProjectSettingsPath).value_or(InputMap{});
        AddDefaultActions(inputMap);
        mWorld.EmplaceResource<InputMap>(std::move(inputMap));

        mWorld.EmplaceResource<RenderSettings>(RenderSettings::LoadFile(mProject.mProjectSettingsPath).value_or(RenderSettings{}));
        mWorld.EmplaceResource<InputState>();
        mWorld.EmplaceResource<EngineModeState>();

        // physcis world - currently Jolt
        mWorld.EmplaceResource<PhysicsWorld>();

        mScheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, *mWindow);

        mScriptSystem = &mScheduler.AddSystem<ScriptSystem>(SystemPhase::PreUpdate, mScriptHost);

        // FixedUpdate: once per fixed substep, before PostUpdate turns the
        // Transforms it writes into world matrices
        mScheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate);

        // should be before any other system in PostUpdate
        mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);
        mModelSystem =
            &mScheduler.AddSystem<ModelSystem>(SystemPhase::PostUpdate, mRenderer, Assets(), mProject.mAssetIdRoot);
        mMeshResolveSystem = &mScheduler.AddSystem<MeshResolveSystem>(SystemPhase::PostUpdate, mRenderer, Assets(),
            mProject.mAssetIdRoot);

        // after TransformPropagateSystem: it reads the WorldTransform that system writes
        mScheduler.AddSystem<AudioEmitterSystem>(SystemPhase::PostUpdate);
        mScheduler.AddSystem<AudioListenerSystem>(SystemPhase::PostUpdate);

        mRenderSystem = &mScheduler.AddSystem<RenderSystem>(SystemPhase::Render, mRenderer);

        mTerrain->Bind(mWorld, mRenderer, mProject);

        if (mDesc.mUIDemo)
            mUIDemo.Load(mGameUI, Assets());

        // scanned once: a new game needs a build to cook its assets anyway
        mAvailableProjects = DiscoverGameProjects();
    #if MIR_WITH_EDITOR
        mProjectMenu.clear();
        for (const GameProject &project : mAvailableProjects)
            mProjectMenu.push_back(ProjectMenuEntry{ project.mDir, project.mName });
    #endif

        mSplash.SetProgress("Loading scene...", 0.9f);
        mScene = mir::NewScene("untitled");
        LoadProjectScene();

        //without the editor there is no Play button
        if (!mDesc.mEnableEditorLayout)
            PlayScene();

        mWindow->Show();
        return true;
    }

    bool App::RequestOpenProject(const std::string &requestedDir)
    {
        if (!mInitialized)
            return false;

        // validate before closing; not ResolveGameProject, which logs "Loading game"
        const std::optional<std::filesystem::path> gameDir = NormalizeGameDir(requestedDir);
        std::optional<GameProject> project = gameDir.has_value() ? LoadGameProject(*gameDir) : std::nullopt;
        if (!project.has_value())
        {
            MIR_LOG_ERROR("App::RequestOpenProject: '{}' is not a game, '{}' stays open", requestedDir, mProject.mDir);
            return false;
        }

        // one game per process: main.cpp relaunches after Run returns
        MIR_LOG_INFO("Closing '{}' to open '{}'", mProject.mDir, project->mDir);
        mProjectToOpen = std::move(project);
        mWindow->RequestClose();
        return true;
    }

    void App::NewScene(std::string name)
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::NewScene: ignored while Play is running (Stop first)");
            return;
        }

        UnloadCurrentScene();
        mScene = mir::NewScene(std::move(name));
    }

    bool App::SaveScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::SaveScene: ignored while Play is running (Stop first)");
            return false;
        }

        return mir::SaveScene(mWorld, mProject.mSceneDir, mScene);
    }

    bool App::LoadScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::LoadScene: ignored while Play is running (Stop first)");
            return false;
        }

        if (!std::filesystem::exists(SceneManifestPath(mProject.mSceneDir)))
        {
            MIR_LOG_ERROR("App::LoadScene: no {} in '{}'", kSceneManifestFileName, mProject.mSceneDir.string());
            return false;
        }

        UnloadCurrentScene();
        mScene = mir::LoadScene(mWorld, mProject.mSceneDir);

        // saved names to script instances
        ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);
        mTerrain->Load();
        return true;
    }

    void App::PlayScene()
    {
        if (IsPlaying(mWorld))
            return;

        // Play pins the terrain releases present on entry
        mTerrain->Refresh(0.0f, true);

        mPlaySnapshot = CaptureWorld(mWorld);

        SetPlayState(mWorld, PlayState::Running);
        SetEngineMode(mWorld, EngineMode::Play);
    }

    void App::StopScene()
    {
        if (!IsPlaying(mWorld))
            return;

        SystemContext context = MakeContext(0.0f);
        mScriptSystem->StopAllScripts(context);

        mScene = RestoreWorld(mWorld, mPlaySnapshot);

        // Play's entities are gone and the restored ones hold no handles yet
        mModelSystem->Reset();
        mMeshResolveSystem->Reset();
        mWorld.Resource<ModelDrawItems>().items.clear();
        mWorld.Resource<ModelMorphOverrides>().instances.clear();
        mTerrain->ForgetMeshes();
        mRenderer.ReleaseSceneResources();

        ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);

        SetEngineMode(mWorld, EngineMode::Edit);
        SetPlayState(mWorld, PlayState::Running);

        // Picks up a release published during Play
        mTerrain->Refresh(0.0f, true);
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorld))
            return;

        SetPlayState(mWorld, IsPaused(mWorld) ? PlayState::Running : PlayState::Paused);
    }

    void App::ApplyShellAction(GameShellAction action)
    {
        switch (action)
        {
        case GameShellAction::Pause:
            if (IsSimulating(mWorld))
                TogglePause();
            break;
        case GameShellAction::Resume:
            if (IsPaused(mWorld))
                TogglePause();
            break;
        case GameShellAction::Restart:
            StopScene();
            PlayScene();
            break;
        case GameShellAction::Quit:
            //the editor's Quit leaves Play, but game build closes
            if (mDesc.mEnableEditorLayout)
                StopScene();
            else
                mWindow->RequestClose();
            break;
        case GameShellAction::None:
            break;
        }
    }

    void App::ToggleFullscreen()
    {
        if (!mInitialized)
            return;

        mWindow->ToggleFullscreen();

        mRenderer.RequestSwapchainRecreate();
    }

    void App::UnloadCurrentScene()
    {
        UnloadScene(mWorld, mScene);

        // no scene entity holds a mesh or material handle now
        if (mModelSystem)
            mModelSystem->Reset();
        if (mMeshResolveSystem)
            mMeshResolveSystem->Reset();
        if (auto *draws = mWorld.TryResource<ModelDrawItems>())
            draws->items.clear();
        if (auto *overrides = mWorld.TryResource<ModelMorphOverrides>())
            overrides->instances.clear();
        mTerrain->Unload();
        mRenderer.ReleaseSceneResources();
    }

    void App::LoadProjectScene()
    {
        std::error_code ec;
        if (std::filesystem::exists(SceneManifestPath(mProject.mSceneDir), ec))
            LoadScene();
        else
            MIR_LOG_INFO("No {} in '{}', starting with an empty scene", kSceneManifestFileName, mProject.mSceneDir.string());
    }

    void App::RunSwitchHook()
    {
        if (mSwitchHookFired || mDesc.mSwitchProjectTo.empty() || mDesc.mMaxFrames == 0)
            return;

        if (mFrame < mDesc.mMaxFrames / 2)
            return;

        // once, even if it fails
        mSwitchHookFired = true;
        RequestOpenProject(mDesc.mSwitchProjectTo);
    }

    AssetCache *App::Assets()
    {
        if (mAssetCache.has_value())
            return &*mAssetCache;

        if (mAssetLoadFailed)
            return nullptr; // already tried and logged; do not re-stat the disk every
                            // call

        const std::filesystem::path manifestPath = CookedAssetsDir() / kAssetManifestFileName;
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

    SystemContext App::MakeContext(float dt) { return SystemContext{ mWorld, mCommands, dt, mElapsed, mFrame }; }

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
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), mDesc.mMaxDeltaSeconds);
            previous = now;
            mElapsed += dt;

        #if MIR_WITH_EDITOR
            mAssetImport.Update(IsEditing(mWorld));
            if (mAssetImport.PendingApply() && IsEditing(mWorld))
                ApplyImportedAssets();
        #endif

            // Every mode, before any system runs
            mTerrain->Update();

            // Edit, between frames, before any system runs
            if (IsEditing(mWorld))
                mTerrain->Refresh(dt, false);

            mImGui.BeginFrame();

            if (mWindow->Width() != 0 && mWindow->Height() != 0)
            {
            #if MIR_WITH_EDITOR
                const EditorActions actions = mEditor.DrawLayout(
                    mDesc.mEnableEditorLayout, mWorld.Resource<InputMap>(), mWorld.Resource<RenderSettings>(),
                    mWorld.Resource<EngineModeState>(), mProjectMenu, mProject.mDir, mWorld, mAssetImport.View(),
                    mRenderer);
            #endif

                mGameUI.BeginFrame(GameUIArea(), mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(),
                    ToUIPointer(mWindow->RawInput()));
                GameShellAction shellAction = GameShellAction::None;
                if (mDesc.mUIDemo)
                    mUIDemo.Draw(mGameUI);
                else
                    shellAction = mGameShell.Draw(mGameUI, IsPlaying(mWorld), dt);
                mGameUI.EndFrame();

                ApplyShellAction(shellAction);

            #if MIR_WITH_EDITOR
                ApplyEditorActions(actions);
            #endif
            }

            mRenderer.SetImGuiDrawData(mImGui.EndFrame());

        #if MIR_WITH_EDITOR
            mRenderSystem->SetViewportRect(0, mDesc.mEnableEditorLayout ? mEditor.SceneViewportRect() : VkRect2D{});

            // Only feed EditorCamera input in Edit.
            if (IsEditing(mWorld) && mDesc.mEnableEditorLayout)
            {
                mEditor.UpdateCamera(dt);
                const EditorCamera &camera = mEditor.SceneCamera();
                mRenderSystem->SetExternalCamera(0, camera.Pose(), camera.mLens);
            }
            else
            {
                mRenderSystem->SetViewportCamera(0, kNullEntity);
            }

            mWorld.Resource<InputState>().SetUiCapture(mEditor.WantsCaptureKeyboard(), mEditor.WantsCaptureMouse());
        #else
            mRenderSystem->SetViewportRect(0, VkRect2D{});
            mRenderSystem->SetViewportCamera(0, kNullEntity);
        #endif

            mScriptReloadWatcher.Poll(Assets(), dt);

            // advances in every mode; FixedUpdate systems check IsSimulating
            // themselves, as ScriptSystem does
            const uint32_t fixedSteps = mFixedTimestep.Advance(dt);

            SystemContext context = MakeContext(dt);
            if (!mDesc.mCapturePath.empty() && mFrame + 1 == mDesc.mMaxFrames)
                mRenderer.CaptureNextFrame(mDesc.mCapturePath);
            mScheduler.Update(context, fixedSteps, mFixedTimestep.mStep);

            if (mWorld.Resource<InputState>().JustPressed(kToggleFullscreenAction))
                ToggleFullscreen();

            audio::Update();

            ++mFrame;

            RunSwitchHook();

            // same exit path as the close button
            if (mDesc.mMaxFrames != 0 && mFrame >= mDesc.mMaxFrames)
                mWindow->RequestClose();
        }
    }

    UIRect App::GameUIArea() const
    {
    #if MIR_WITH_EDITOR
        if (mDesc.mEnableEditorLayout)
            return ToUIRect(mEditor.SceneViewportRect());
    #endif
        return UIRect{ 0.0f, 0.0f, static_cast<float>(mWindow->Width()), static_cast<float>(mWindow->Height()) };
    }

#if MIR_WITH_EDITOR
    void App::ApplyEditorActions(const EditorActions &actions)
    {
        switch (actions.mScene)
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
        case SceneMenuAction::Play:
            PlayScene();
            break;
        case SceneMenuAction::TogglePause:
            TogglePause();
            break;
        case SceneMenuAction::Stop:
            StopScene();
            break;
        case SceneMenuAction::None:
            break;
        }

        ApplyEntityAction(actions.mEntity);

        bool assetActionAccepted = true;
        if (actions.mAssetImport.mKind == AssetImportActionKind::Remove)
        {
            AssetId target = MakeAssetId(actions.mAssetImport.mModelPath);
            for (const auto &model : mAssetImport.View().mModels)
                if (model.mPath == actions.mAssetImport.mModelPath && model.mAssetId != 0)
                    target = AssetId{ model.mAssetId };
            mWorld.GetOrCreateQuery<const ModelPlayer>().ForEach([&](Entity, const ModelPlayer &player)
                {
                    AssetId referenced = player.ModelAsset();
                    if (referenced.IsNull() && !player.ModelName().empty())
                        if (const AssetPathResult logical =
                            MakeLogicalAssetPath(mProject.mAssetIdRoot, player.ModelName());
                            logical.Ok())
                            referenced = MakeAssetId(logical.path);
                    assetActionAccepted &= referenced != target;
                });
            if (!assetActionAccepted)
                mAssetImport.Reject(actions.mAssetImport.mModelPath,
                    "cannot remove this asset while a scene entity references it");
        }
        if (assetActionAccepted)
            mAssetImport.Handle(actions.mAssetImport);

        if (!actions.mOpenProject.empty())
            RequestOpenProject(actions.mOpenProject);

        if (!actions.mCreateProjectName.empty() &&
            CreateGameProject(actions.mCreateProjectName, actions.mCreateProjectRayTracing))
        {
            RequestOpenProject("games/" + actions.mCreateProjectName);
        }
    }

    void App::ApplyEntityAction(const EntityAction &action)
    {
        Selection &selection = mWorld.Resource<Selection>();

        switch (action.mKind)
        {
        case EntityActionKind::Create:
            {
                const EntityPresetDesc *preset = action.mPreset;
                if (preset != nullptr && preset->mCanCreate != nullptr && !preset->mCanCreate(mWorld))
                    return;

                const Entity entity = CreateSceneEntity(mWorld, mScene);
                if (preset != nullptr)
                    preset->mCreate(mWorld, entity);

                // after the preset
                AddMissingBaseline(mWorld, entity);

                selection.mSelected = { entity };
                break;
            }
        case EntityActionKind::Delete:
            {
                const Entity neighbor = NeighborInHierarchy(mWorld, action.mTarget);
                if (DestroySceneEntity(mWorld, mScene, action.mTarget) && selection.Contains(action.mTarget))
                    selection.mSelected = neighbor.IsNull() ? std::vector<Entity>{} : std::vector<Entity>{ neighbor };
                break;
            }
        case EntityActionKind::Duplicate:
            if (const Entity copy = DuplicateSceneEntity(mWorld, mScene, action.mTarget); !copy.IsNull())
                selection.mSelected = { copy };
            break;
        case EntityActionKind::None:
            break;
        }
    }

    void App::ApplyImportedAssets()
    {
        auto candidateManifest = AssetManifest::LoadFile(CookedAssetsDir() / kAssetManifestFileName);
        if (!candidateManifest)
        {
            mAssetImport.MarkApplyFailed("the new asset manifest could not be loaded");
            return;
        }
        AssetCache candidate(&*candidateManifest, CookedAssetsDir());
        std::string error;
        if (mModelSystem && !mModelSystem->PrepareReload(candidate, error))
        {
            mAssetImport.MarkApplyFailed("new model resources were rejected: " + error);
            return;
        }

        mAssetCache.reset();
        mAssetManifest = std::move(candidateManifest);
        mAssetCache.emplace(&*mAssetManifest, CookedAssetsDir());
        if (mModelSystem)
            mModelSystem->CommitReload(&*mAssetCache);
        mAssetLoadFailed = false;
        mAssetImport.MarkApplied();
        MIR_LOG_INFO("Applied imported assets at an Edit frame boundary");
    }
#endif

    void App::Shutdown()
    {
        if (!mInitialized)
        {
            return;
        }

        // let all the systems stop first
        SystemContext stopContext = MakeContext(0.0f);
        mScheduler.Stop(stopContext);

    #if MIR_WITH_EDITOR
        mAssetImport.Shutdown();
    #endif

        audio::Terminate();

        mScheduler.Reset();

        // Cache before manifest
        mAssetCache.reset();
        mAssetManifest.reset();
        mAssetLoadFailed = false;

        DiscardSnapshot(mPlaySnapshot);

    #if MIR_WITH_EDITOR
        mEditor.Shutdown();
    #endif
        mImGui.Shutdown(mRenderer);

        mRenderer.Shutdown();
        mWindow.reset();
        mInitialized = false;
        MIR_LOG_INFO("App shut down");
    }
} // namespace mir
