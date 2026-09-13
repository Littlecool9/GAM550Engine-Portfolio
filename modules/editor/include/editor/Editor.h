/**
 * @file Editor.h
 * @author Sumin Park
 * @brief Owns the ImGui context, backend, and the Slate editor shell
 *        (dockspace, Hierarchy/Inspector/Output panels, Debug menu).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/EngineMode.h>
#include <core/platform/Surface.h>
#include <editor/EditorCamera.h>
#include <editor/panels/LogPanel.h>
#include <editor/panels/ProjectSettingsPanel.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <filesystem>
#include <string>
#include <vector>

struct ImDrawData;

namespace mir
{
    enum class SceneMenuAction
    {
        None,
        New,
        Save,
        Load,
        Play,
        TogglePause,
        Stop,
    };

    class Editor
    {
    public:
        Editor() = default;
        ~Editor();

        Editor(const Editor &) = delete;
        Editor &operator=(const Editor &) = delete;
        Editor(Editor &&) = delete;
        Editor &operator=(Editor &&) = delete;

        // `renderer`'s device and render target must already be created.
        bool Initialize(Window &window, VulkanRenderer &renderer, std::filesystem::path projectSettingsPath);

        // Must run before renderer.Shutdown() and before the window is
        // destroyed. No-op when not initialized.
        void Shutdown(VulkanRenderer &renderer);

        // Call once per frame, before DrawLayout or any editor drawing.
        void BeginFrame();

        // enableLayout = false disables editor
        // `mode` drives the Play/Pause/Stop toolbar's labels and which of them are enabled
        SceneMenuAction DrawLayout(bool enableLayout, InputMap &inputMap, const EngineModeState &mode);

        ImDrawData *EndFrame();

        // The dockspace's central node, where the 3D scene shows through.
        VkRect2D SceneViewportRect() const { return mSceneViewportRect; }


        void UpdateCamera(float dt);

        EditorCamera &SceneCamera() { return mCamera; }

        bool IsInitialized() const { return mInitialized; }

        bool WantsCaptureKeyboard() const;
        bool WantsCaptureMouse() const;

    private:
        void DrawTitleBar();
        // Returns Play/TogglePause/Stop if one of the toolbar's buttons was
        // clicked this frame, else None.
        SceneMenuAction DrawPlayControls(const EngineModeState &mode);

        bool mInitialized = false;
        bool mShowStyleEditor = false;
        bool mShowProjectSettings = false;
        std::string mImGuiIniPath;
        std::filesystem::path mProjectSettingsPath;
        VkRect2D mSceneViewportRect{};
        Window *mWindow = nullptr;
        std::vector<PixelRect> mTitleBarInteractiveRects;
        LogPanel mLogPanel;
        ProjectSettingsPanel mProjectSettingsPanel;
        EditorCamera mCamera;
    };
}
