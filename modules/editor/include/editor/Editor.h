/**
 * @file Editor.h
 * @author Sumin Park
 * @brief The Slate editor shell (dockspace, Hierarchy/Inspector/Output
 *        panels, Debug menu), drawn into the context ImGuiRuntime owns.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/EngineMode.h>
#include <core/platform/Surface.h>
#include <editor/EditorCamera.h>
#include <editor/panels/AssetImportPanel.h>
#include <editor/panels/ContentBrowserPanel.h>
#include <editor/panels/HierarchyPanel.h>
#include <editor/panels/InputSettingsPanel.h>
#include <editor/panels/InspectorPanel.h>
#include <editor/panels/LogPanel.h>
#include <editor/panels/ProfilerPanel.h>
#include <editor/panels/ProjectSettingsPanel.h>
#include <imguiruntime/ImGuiRuntime.h>
#include <renderer/RenderSettings.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    enum class GizmoSpace
    {
        Local,
        World,
    };

    struct ProjectMenuEntry
    {
        std::string mDir;
        std::string mName;
    };

    struct EditorActions
    {
        SceneMenuAction mScene = SceneMenuAction::None;
        EntityAction mEntity;
        std::string mOpenProject;

        // non-empty mCreateProjectName requests CreateGameProject(name, RT),
        // then the same open-into-new-directory path as mOpenProject
        std::string mCreateProjectName;
        bool mCreateProjectRayTracing = false;
        AssetImportAction mAssetImport;
        ContentBrowserAction mContentBrowser;
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

        // imgui must already be initialized; the editor never owns the context
        bool Initialize(const ImGuiRuntime &imgui, Window &window, std::filesystem::path projectSettingsPath,
                        World &world);

        //before ImGuiRuntime::Shutdown
        void Shutdown();

        //between ImGuiRuntime::BeginFrame and EndFrame
        EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                                 const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                                 std::string_view currentProject, World &world, const AssetImportView &assetImport,
                                 const VulkanRenderer &renderer, const ContentBrowserView &contentBrowser);

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
        // F moves the scene camera next to the primary selection.
        void FocusSelection(const EngineModeState &mode, World &world);
        void DrawSelectionHighlights(const EngineModeState &mode, World &world);
        void DrawComponentGizmos(const EngineModeState &mode, World &world);
        void DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects);

        SceneMenuAction DrawPlayControls(const EngineModeState &mode);

        bool mInitialized = false;
        bool mShowStyleEditor = false;
        bool mShowProjectSettings = false;
        bool mShowInputSettings = false;
        bool mShowAssetImport = false;
        bool mShowContentBrowser = false;
        bool mShowProfiler = false;
        std::string mImGuiIniPath;
        std::filesystem::path mProjectSettingsPath;
        VkRect2D mSceneViewportRect{};
        Window *mWindow = nullptr;
        std::vector<PixelRect> mTitleBarInteractiveRects;
        LogPanel mLogPanel;
        HierarchyPanel mHierarchyPanel;
        AssetImportPanel mAssetImportPanel;
        ContentBrowserPanel mContentBrowserPanel;
        InspectorPanel mInspectorPanel;
        ProjectSettingsPanel mProjectSettingsPanel;
        InputSettingsPanel mInputSettingsPanel;
        ProfilerPanel mProfilerPanel;
        EditorCamera mCamera;

        // waiting on the confirm dialog
        ProjectMenuEntry mPendingProject;
        bool mOpenConfirmRequested = false;

        GizmoOperation mGizmoOperation = GizmoOperation::Translate;
        GizmoSpace mGizmoSpace = GizmoSpace::Local;

        bool mMouseWasDown = false;

        // New Project modal
        bool mNewProjectRequested = false;
        char mNewProjectName[64] = {};
        bool mNewProjectRayTracing = false;
    };
} // namespace mir
