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
#include <editor/panels/HierarchyPanel.h>
#include <editor/panels/InspectorPanel.h>
#include <editor/panels/LogPanel.h>
#include <editor/panels/ProjectSettingsPanel.h>
#include <renderer/RenderSettings.h>
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
        Play,
        TogglePause,
        Stop,
    };

    enum class GizmoOperation
    {
        Translate,
        Rotate,
        Scale,
    };

    struct ProjectMenuEntry
    {
        std::string mDir;
        std::string mName;
    };

    struct EditorActions
    {
        SceneMenuAction mScene = SceneMenuAction::None;
        std::string mOpenProject;

        // non-empty mCreateProjectName requests CreateGameProject(name, RT),
        // then the same open-into-new-directory path as mOpenProject
        std::string mCreateProjectName;
        bool mCreateProjectRayTracing = false;
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

        bool Initialize(Window &window, VulkanRenderer &renderer, std::filesystem::path projectSettingsPath, World &world);

        void Shutdown(VulkanRenderer &renderer);

        void BeginFrame();

        EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                                 const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                                 std::string_view currentProject, World &world);

        ImDrawData *EndFrame();

        VkRect2D SceneViewportRect() const { return mSceneViewportRect; }

        void UpdateCamera(float dt);

        EditorCamera &SceneCamera() { return mCamera; }

        bool IsInitialized() const { return mInitialized; }

        bool WantsCaptureKeyboard() const;
        bool WantsCaptureMouse() const;

    private:
        void DrawTitleBar();
        void DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject);
        void DrawOpenProjectConfirm(EditorActions &actions);
        void DrawViewportGizmo(const EngineModeState &mode, World &world);
        void UpdateViewportPicking(const EngineModeState &mode, World &world);
        void DrawSelectionHighlights(const EngineModeState &mode, World &world);
        void DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects);

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
        HierarchyPanel mHierarchyPanel;
        InspectorPanel mInspectorPanel;
        ProjectSettingsPanel mProjectSettingsPanel;
        EditorCamera mCamera;

        // waiting on the confirm dialog
        ProjectMenuEntry mPendingProject;
        bool mOpenConfirmRequested = false;

        GizmoOperation mGizmoOperation = GizmoOperation::Translate;

        bool mMouseWasDown = false;

        // New Project modal
        bool mNewProjectRequested = false;
        char mNewProjectName[64] = {};
        bool mNewProjectRayTracing = false;
    };
}
