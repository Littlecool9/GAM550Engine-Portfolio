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

#include <core/platform/Surface.h>
#include <editor/panels/LogPanel.h>
#include <editor/panels/ProjectSettingsPanel.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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
    };

    // One game in Project > Open Project
    struct ProjectMenuEntry
    {
        // e.g. "games/HelloWorld"
        std::string mDir;

        // shown in the menu
        std::string mName;
    };

    // What the menus asked for this frame
    struct EditorActions
    {
        SceneMenuAction mScene = SceneMenuAction::None;

        // confirmed game directory to open; empty for none
        std::string mOpenProject;
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

        // Draws the editor shell and returns what the menus asked for this frame.
        // enableLayout = false disables editor
        EditorActions DrawLayout(bool enableLayout, InputMap &inputMap,
                                 std::span<const ProjectMenuEntry> projects, std::string_view currentProject);

        // Returns draw data for VulkanRenderer::SetImGuiDrawData.
        // BeginFrame is needed regardless of skipping layout
        ImDrawData *EndFrame();

        // The dockspace's central node, where the 3D scene shows through.
        VkRect2D SceneViewportRect() const { return mSceneViewportRect; }

        bool IsInitialized() const { return mInitialized; }

        bool WantsCaptureKeyboard() const;
        bool WantsCaptureMouse() const;

    private:
        void DrawTitleBar();
        void DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject);
        void DrawOpenProjectConfirm(EditorActions &actions);

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

        // waiting on the confirm dialog
        ProjectMenuEntry mPendingProject;
        bool mOpenConfirmRequested = false;
    };
}
