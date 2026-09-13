/**
 * @file App.cpp
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>
#include <core/EngineVersion.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/RenderSystem.h>
#include <renderer/Shapes.h>
#include <renderer/components/MeshRenderer.h>
#include <scene/SceneIO.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptSystem.h>
#include <script/components/ScriptRef.h>

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <unordered_map>

namespace mir
{
    namespace
    {

        struct GameProject
        {
            std::string mTitle = "MjolnirEngine";
            std::string mAssetsRoot = "assets";
            std::filesystem::path mSceneDir = "scenes/default";
        };

        GameProject LoadGameProject(const std::filesystem::path &path)
        {
            GameProject project;

            std::ifstream in(path);
            if (!in)
            {
                MIR_LOG_ERROR("LoadGameProject: could not open '{}', using defaults", path.string());
                return project;
            }

            nlohmann::json json;
            try
            {
                in >> json;
            }
            catch (const nlohmann::json::parse_error &e)
            {
                MIR_LOG_ERROR("LoadGameProject: '{}' is not valid JSON ({}), using defaults", path.string(), e.what());
                return project;
            }

            project.mTitle = json.value("title", project.mTitle);
            project.mAssetsRoot = json.value("assetsRoot", project.mAssetsRoot);
            project.mSceneDir = json.value("sceneDir", project.mSceneDir.string());
            return project;
        }

        std::optional<MeshData> ParseMeshAsset(std::string_view text, std::string_view assetPath)
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

            MeshData mesh;
            for (const auto &v : json.value("vertices", nlohmann::json::array()))
            {
                Vertex vertex{};
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
    }

    App::~App()
    {
        Shutdown();
    }

    bool App::Initialize(const AppDesc &desc)
    {
        mDesc = desc;

        std::filesystem::path projectPath = mDesc.mProjectPath;
        if (projectPath.is_relative())
            projectPath = ExecutableDir() / projectPath;
        const GameProject project = LoadGameProject(projectPath);

        mSceneDir = project.mSceneDir;
        mAssetsRoot = project.mAssetsRoot;

        if (mSceneDir.is_relative())
            mSceneDir = ExecutableDir() / mSceneDir;
        if (mDesc.mProjectSettingsPath.is_relative())
            mDesc.mProjectSettingsPath = ExecutableDir() / mDesc.mProjectSettingsPath;

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
        windowDesc.mTitle = project.mTitle.c_str();
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
        if (!mRenderer.Initialize({ .window = mWindow.get(),
                                   .appName = desc.mAppName,
                                   .enableValidation = desc.mEnableValidation }))
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
        mWorld.EmplaceResource<FrameCommands>(FrameCommands{ &mCommands });

        mWorld.EmplaceResource<InputMap>(InputMap::LoadFile(mDesc.mProjectSettingsPath));
        mWorld.EmplaceResource<InputState>();
        mWorld.EmplaceResource<EngineModeState>();

        mScheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, *mWindow);

        mScriptSystem = &mScheduler.AddSystem<ScriptSystem>(SystemPhase::PreUpdate, mScriptHost);

        // should be before any other system in PostUpdate
        mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);

        mRenderSystem = &mScheduler.AddSystem<RenderSystem>(SystemPhase::Render, mRenderer);

        mSplash.SetProgress("Loading scene...", 0.9f);
        mScene = mir::NewScene("untitled");

        mWindow->Show();

        return true;
    }

    void App::NewScene(std::string name)
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::NewScene: ignored while Play is running (Stop first)");
            return;
        }

        UnloadScene(mWorld, mScene);
        mScene = mir::NewScene(std::move(name));
    }

    bool App::SaveScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::SaveScene: ignored while Play is running (Stop first)");
            return false;
        }

        return mir::SaveScene(mWorld, mSceneDir, mScene);
    }

    bool App::LoadScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::LoadScene: ignored while Play is running (Stop first)");
            return false;
        }

        // scene hardcoded at the moment
        if (!std::filesystem::exists(mSceneDir / "scene.json"))
        {
            MIR_LOG_ERROR("App::LoadScene: no scene.json in '{}'", mSceneDir.string());
            return false;
        }

        UnloadScene(mWorld, mScene);
        mScene = mir::LoadScene(mWorld, mSceneDir);

        ResolveSceneScripts();
        ResolveSceneMeshes();

        return true;
    }

    bool App::LoadScriptAsset(std::string_view assetPath, std::string_view scriptName)
    {
        AssetCache *cache = Assets();
        if (cache == nullptr)
            return false;

        const AssetId id = MakeAssetId(assetPath);
        const AssetBlobView *blob = cache->Load(id);
        if (blob == nullptr)
            return false;

        if (!mScriptHost.LoadScriptSource(scriptName, AsStringView(*blob)))
            return false;

        mScriptReloadWatcher.Track(std::string(scriptName), id);
        return true;
    }

    void App::ResolveSceneScripts()
    {
        mWorld.GetOrCreateQuery<ScriptRef>().ForEach(
            [this](Entity, ScriptRef &ref)
            {
                if (ref.instanceRef >= 0 || ref.scriptName[0] == '\0')
                    return;

                const std::string name = ref.scriptName;
                const std::string assetPath = mAssetsRoot + "/scripts/" + name + ".lua";

                if (!LoadScriptAsset(assetPath, name))
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: could not load '{}' for script '{}'", assetPath, name);
                    return;
                }

                ref.instanceRef = mScriptHost.CreateInstance(name);
            });
    }

    void App::ResolveSceneMeshes()
    {
        AssetCache *cache = Assets();

        std::unordered_map<uint64_t, MeshHandle> meshCache;
        std::unordered_map<std::string, MaterialHandle> materialCache;

        mWorld.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity, MeshRenderer &renderer)
            {
                if (renderer.mesh.IsNull() && renderer.meshName[0] != '\0')
                {
                    const std::string assetPath = mAssetsRoot + "/" + renderer.meshName;
                    const AssetId id = MakeAssetId(assetPath);

                    if (auto it = meshCache.find(id.value); it != meshCache.end())
                    {
                        renderer.mesh = it->second;
                    }
                    else if (cache != nullptr)
                    {
                        if (const AssetBlobView *blob = cache->Load(id); blob != nullptr)
                        {
                            if (auto mesh = ParseMeshAsset(AsStringView(*blob), assetPath))
                            {
                                renderer.mesh = mRenderer.CreateMesh(mesh->vertices, mesh->indices);
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
                        .emplace(shaderName, mRenderer.CreateMaterial(
                            MaterialDesc{ .shaderName = shaderName }))
                        .first->second;
                }
            });
    }

    void App::PlayScene()
    {
        if (IsPlaying(mWorld))
            return;

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

        ResolveSceneScripts();
        ResolveSceneMeshes();

        SetEngineMode(mWorld, EngineMode::Edit);
        SetPlayState(mWorld, PlayState::Running);
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorld))
            return;

        SetPlayState(mWorld, IsPaused(mWorld) ? PlayState::Running : PlayState::Paused);
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
        return SystemContext{ mWorld, mCommands, dt, mElapsed, mFrame };
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
                switch (mEditor.DrawLayout(mDesc.mEnableEditorLayout, mWorld.Resource<InputMap>(),
                    mWorld.Resource<EngineModeState>()))
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
            }

            mRenderer.SetImGuiDrawData(mEditor.EndFrame());

            mRenderSystem->SetViewportRect(0, mEditor.SceneViewportRect());

            // Only feed EditorCamera input in Edit.
            if (IsEditing(mWorld))
            {
                mEditor.UpdateCamera(dt);
                mRenderSystem->SetExternalCamera(0, mEditor.SceneCamera().Matrix(), mEditor.SceneCamera().mLens);
            }
            else
            {
                mRenderSystem->SetViewportCamera(0, kNullEntity);
            }

            mWorld.Resource<InputState>().SetUiCapture(mEditor.WantsCaptureKeyboard(), mEditor.WantsCaptureMouse());

            mScriptReloadWatcher.Poll(Assets(), dt);

            SystemContext context = MakeContext(dt);
            mScheduler.Update(context);

            ++mFrame;
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
