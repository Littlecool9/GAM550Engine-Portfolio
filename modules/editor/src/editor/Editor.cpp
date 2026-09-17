/**
 * @file Editor.cpp
 * @author Sumin Park
 * @brief Owns the ImGui context, backend, and the Slate editor shell
 *        (dockspace, Hierarchy/Inspector/Output panels, Debug menu).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/Editor.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <editortheme/EditorTheme.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <algorithm>
#include <utility>

namespace mir
{
    // Shutdown needs a VulkanRenderer this doesn't have, so skipping it leaks
    // the ImGui context rather than crashing into a dangling device.
    Editor::~Editor() {}

    bool Editor::Initialize(Window &window, VulkanRenderer &renderer, std::filesystem::path projectSettingsPath)
    {
        mProjectSettingsPath = std::move(projectSettingsPath);

        ImGui::CreateContext();

        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        mImGuiIniPath = (ExecutableDir() / "imgui.ini").string();
        io.IniFilename = mImGuiIniPath.c_str();

        ImFontConfig fontConfig;
        fontConfig.OversampleH = 3;
        const std::string fontPath = FontPath("Inter.ttf").string();
        if (io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig) == nullptr)
        {
            MIR_LOG_ERROR("Failed to load font: {}", fontPath);
            io.Fonts->AddFontDefault();
        }

        static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = 16.0f;
        const std::string iconFontPath = FontPath(FONT_ICON_FILE_NAME_FAS).string();
        if (io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), 16.0f, &iconConfig, iconRanges) == nullptr)
            MIR_LOG_ERROR("Failed to load icon font: {}", iconFontPath);

        EditorTheme::Apply();
        EditorTheme::ScaleForDpi(window.ContentScale());

        void *nativeHandle = window.NativeHandleForImGui();
        if (nativeHandle == nullptr)
        {
            MIR_LOG_ERROR("Window backend has no native handle for ImGui");
            ImGui::DestroyContext();
            return false;
        }

        if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow *>(nativeHandle), true))
        {
            MIR_LOG_ERROR("ImGui_ImplGlfw_InitForVulkan failed");
            ImGui::DestroyContext();
            return false;
        }

        if (!renderer.InitImGuiVulkanBackend())
        {
            MIR_LOG_ERROR("ImGui Vulkan backend initialization failed");
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        mWindow = &window;
        mInitialized = true;
        return true;
    }

    void Editor::Shutdown(VulkanRenderer &renderer)
    {
        if (!mInitialized)
            return;

        renderer.ShutdownImGuiVulkanBackend();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        mWindow = nullptr;
        mInitialized = false;
    }

    void Editor::BeginFrame()
    {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void Editor::DrawTitleBar()
    {
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float scale = mWindow->ContentScale();
        const float barHeight = Window::kTitleBarHeightDip * scale;

        if (!ImGui::BeginViewportSideBar("##TitleBar", viewport, ImGuiDir_Up, barHeight,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End();
            return;
        }

        ImGui::SetCursorPosY((barHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(mWindow->Title());

        mTitleBarInteractiveRects.clear();
        const float buttonWidth = barHeight * 0.9f;
        const float buttonHeight = barHeight * 0.65f;
        const float buttonY = (barHeight - buttonHeight) * 0.5f;
        const float rowWidth = buttonWidth * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rowWidth);

        ImGui::SetWindowFontScale(0.65f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        const auto controlButton = [&](const char *icon)
            {
                ImGui::PushID(icon);
                ImGui::SetCursorPosY(buttonY);
                const bool clicked = ImGui::Button(icon, ImVec2(buttonWidth, buttonHeight));
                const ImVec2 rMin = ImGui::GetItemRectMin();
                const ImVec2 rMax = ImGui::GetItemRectMax();
                mTitleBarInteractiveRects.push_back(PixelRect{
                    static_cast<int32_t>(rMin.x), static_cast<int32_t>(rMin.y),
                    static_cast<uint32_t>(rMax.x - rMin.x), static_cast<uint32_t>(rMax.y - rMin.y) });
                ImGui::PopID();
                ImGui::SameLine();
                return clicked;
            };

        if (controlButton(ICON_FA_WINDOW_MINIMIZE))
            mWindow->Minimize();
        if (controlButton(mWindow->IsMaximized() ? ICON_FA_WINDOW_RESTORE : ICON_FA_WINDOW_MAXIMIZE))
            mWindow->ToggleMaximize();
        if (controlButton(ICON_FA_XMARK))
            mWindow->RequestClose();

        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);

        mWindow->SetTitleBarInteractiveRects(mTitleBarInteractiveRects);

        ImGui::End();
    }

    SceneMenuAction Editor::DrawPlayControls(const EngineModeState &mode)
    {
        SceneMenuAction action = SceneMenuAction::None;

        ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float height = 28.0f * mWindow->ContentScale();

        if (!ImGui::BeginViewportSideBar("##PlayControls", viewport, ImGuiDir_Up, height,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End();
            return action;
        }

        const bool playing = mode.mMode == EngineMode::Play;
        const bool paused = playing && mode.mPlayState == PlayState::Paused;

        ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);

        if (!playing)
        {
            if (ImGui::Button(ICON_FA_PLAY " Play"))
                action = SceneMenuAction::Play;
        }
        else
        {
            if (ImGui::Button(paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
                action = SceneMenuAction::TogglePause;
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_STOP " Stop"))
                action = SceneMenuAction::Stop;
        }

        ImGui::End();
        return action;
    }

    void Editor::DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject)
    {
        if (!ImGui::BeginMenu("Open Project", !projects.empty()))
            return;

        for (const ProjectMenuEntry &project : projects)
        {
            // ID by directory: names may repeat
            ImGui::PushID(project.mDir.c_str());

            const bool isCurrent = project.mDir == currentProject;
            if (ImGui::MenuItem(project.mName.c_str(), project.mDir.c_str(), isCurrent) && !isCurrent)
            {
                mPendingProject = project;
                mOpenConfirmRequested = true;
            }

            ImGui::PopID();
        }

        ImGui::EndMenu();
    }

    void Editor::DrawOpenProjectConfirm(EditorActions &actions)
    {
        constexpr const char *kPopupId = "Open Project##Confirm";

        // outside the menu: OpenPopup needs the same ID stack as BeginPopupModal
        if (mOpenConfirmRequested)
        {
            ImGui::OpenPopup(kPopupId);
            mOpenConfirmRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPopupId, nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::Text("Open \"%s\"?", mPendingProject.mName.c_str());
        ImGui::TextDisabled("The editor restarts with this game.");
        ImGui::TextDisabled("Unsaved changes to the current scene will be lost.");
        ImGui::Spacing();

        if (ImGui::Button("Open"))
        {
            actions.mOpenProject = mPendingProject.mDir;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    EditorActions Editor::DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                                     const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                                     std::string_view currentProject)
    {
        EditorActions actions;

        if (enableLayout)
        {
            if (mWindow->HasCustomTitleBar())
                DrawTitleBar();

            actions.mScene = DrawPlayControls(mode);

            const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(
                0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

            ImGuiDockNode *dockspaceNode = ImGui::DockBuilderGetNode(dockspaceId);
            if (dockspaceNode != nullptr && dockspaceNode->IsEmpty())
            {
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID center = dockspaceId;
                const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
                const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
                const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

                ImGui::DockBuilderDockWindow("Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", right);
                ImGui::DockBuilderDockWindow("Output", bottom);

                ImGui::DockBuilderFinish(dockspaceId);
            }

            for (const char *name :{ "Hierarchy", "Inspector" })
            {
                ImGui::Begin(name);
                ImGui::End();
            }

            if (ImGui::Begin("Output"))
                mLogPanel.Draw();
            ImGui::End();

            if (const ImGuiDockNode *centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId);
                centralNode != nullptr)
            {
                mSceneViewportRect = VkRect2D{
                    .offset{ static_cast<int32_t>(centralNode->Pos.x), static_cast<int32_t>(centralNode->Pos.y) },
                    .extent{ static_cast<uint32_t>(std::max(centralNode->Size.x, 0.0f)),
                            static_cast<uint32_t>(std::max(centralNode->Size.y, 0.0f)) } };
            }

            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    ImGui::BeginDisabled(mode.mMode == EngineMode::Play);

                    if (ImGui::MenuItem("New Scene"))
                        actions.mScene = SceneMenuAction::New;
                    if (ImGui::MenuItem("Save Scene"))
                        actions.mScene = SceneMenuAction::Save;
                    if (ImGui::MenuItem("Load Scene"))
                        actions.mScene = SceneMenuAction::Load;

                    ImGui::EndDisabled();
                    ImGui::Separator();
                    DrawOpenProjectMenu(projects, currentProject);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Project"))
                {
                    ImGui::MenuItem("Project Settings", nullptr, &mShowProjectSettings);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Debug"))
                {
                    ImGui::MenuItem("Style Editor", nullptr, &mShowStyleEditor);
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            DrawOpenProjectConfirm(actions);

            if (mShowStyleEditor)
            {
                if (ImGui::Begin("Style Editor", &mShowStyleEditor))
                    ImGui::ShowStyleEditor();
                ImGui::End();
            }

            if (mShowProjectSettings)
            {
                if (ImGui::Begin("Project Settings", &mShowProjectSettings))
                    mProjectSettingsPanel.Draw(inputMap, renderSettings, mWindow->RawInput(), mProjectSettingsPath);
                else
                    mProjectSettingsPanel.CancelListening();
                ImGui::End();
            }
            else
            {
                mProjectSettingsPanel.CancelListening();
            }
        }
        else
        {
            mSceneViewportRect = VkRect2D{};
        }

        return actions;
    }

    ImDrawData *Editor::EndFrame()
    {
        ImGui::Render();
        return ImGui::GetDrawData();
    }

    bool Editor::WantsCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
    bool Editor::WantsCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }

    void Editor::UpdateCamera(float dt)
    {
        const bool lookHeld = !WantsCaptureMouse() && mWindow->RawInput().IsDown(MouseButton::Right);
        mCamera.Update(mWindow->RawInput(), lookHeld, dt);
    }
}
