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

#include <core/ecs/Query.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <editor/Selection.h>
#include <editortheme/EditorTheme.h>
#include <renderer/CameraMath.h>
#include <renderer/components/MeshBounds.h>
#include <renderer/components/MeshRenderer.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace mir
{
    namespace
    {
        ImGuizmo::OPERATION ToImGuizmoOperation(GizmoOperation operation)
        {
            switch (operation)
            {
            case GizmoOperation::Translate:
                return ImGuizmo::TRANSLATE;
            case GizmoOperation::Rotate:
                return ImGuizmo::ROTATE;
            case GizmoOperation::Scale:
                return ImGuizmo::SCALE;
            }
            return ImGuizmo::TRANSLATE;
        }

        void DecomposeTRS(const glm::mat4 &matrix, glm::vec3 &position, glm::quat &rotation, glm::vec3 &scale)
        {
            position = glm::vec3(matrix[3]);
            scale = { glm::length(glm::vec3(matrix[0])), glm::length(glm::vec3(matrix[1])),
                      glm::length(glm::vec3(matrix[2])) };

            const glm::mat3 rotationMatrix(glm::vec3(matrix[0]) / (scale.x != 0.0f ? scale.x : 1.0f),
                                           glm::vec3(matrix[1]) / (scale.y != 0.0f ? scale.y : 1.0f),
                                           glm::vec3(matrix[2]) / (scale.z != 0.0f ? scale.z : 1.0f));
            rotation = glm::quat_cast(rotationMatrix);
        }
    }

    // Shutdown needs a VulkanRenderer this doesn't have, so skipping it leaks
    // the ImGui context rather than crashing into a dangling device.
    Editor::~Editor() {}

    bool Editor::Initialize(Window &window, VulkanRenderer &renderer, std::filesystem::path projectSettingsPath,
                            World &world)
    {
        mProjectSettingsPath = std::move(projectSettingsPath);

        world.EmplaceResource<Selection>();

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

    void Editor::DrawViewportGizmo(const EngineModeState &mode, World &world)
    {
        // matches EditorCamera's own Edit-only gating in App::Run
        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const Selection *selection = world.TryResource<Selection>();
        const Entity entity = selection != nullptr ? selection->Primary() : kNullEntity;
        if (entity.IsNull() || !world.IsAlive(entity))
            return;

        Transform *transform = world.GetComponent<Transform>(entity);
        if (transform == nullptr)
            return;

        const float x = static_cast<float>(mSceneViewportRect.offset.x);
        const float y = static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);

        ImGui::SetNextWindowPos(ImVec2(x + 8.0f, y + 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        if (ImGui::Begin("##GizmoToolbar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing))
        {
            const auto modeButton = [&](const char *label, GizmoOperation op)
                {
                    const bool active = mGizmoOperation == op;
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(label))
                        mGizmoOperation = op;
                    if (active)
                        ImGui::PopStyleColor();
                };

            modeButton("Translate", GizmoOperation::Translate);
            ImGui::SameLine();
            modeButton("Rotate", GizmoOperation::Rotate);
            ImGui::SameLine();
            modeButton("Scale", GizmoOperation::Scale);
        }
        ImGui::End();

        const glm::mat4 view = glm::inverse(mCamera.Matrix());
        const glm::mat4 proj = glm::perspective(glm::radians(mCamera.mLens.mFovYDegrees), width / height,
                                                mCamera.mLens.mNearZ, mCamera.mLens.mFarZ);

        glm::mat4 worldMatrix = ResolveWorld(world, entity);

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::BeginFrame();
        ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList(ImGui::GetMainViewport()));
        ImGuizmo::SetRect(x, y, width, height);

        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ToImGuizmoOperation(mGizmoOperation),
                                 ImGuizmo::LOCAL, glm::value_ptr(worldMatrix)))
        {
            const Entity parent = ParentOf(world, entity);
            const glm::mat4 localMatrix =
                parent.IsNull() ? worldMatrix : glm::inverse(ResolveWorld(world, parent)) * worldMatrix;

            glm::vec3 position{};
            glm::vec3 scale{};
            glm::quat rotation{};
            DecomposeTRS(localMatrix, position, rotation, scale);

            transform->SetPosition(position);
            transform->SetRotation(rotation);
            transform->SetScale(scale);
        }
    }

    void Editor::UpdateViewportPicking(const EngineModeState &mode, World &world)
    {
        const bool down = mWindow->RawInput().IsDown(MouseButton::Left);
        const bool justPressed = down && !mMouseWasDown;
        mMouseWasDown = down;

        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        if (WantsCaptureMouse())
            return;

        if (ImGuizmo::IsOver() || ImGuizmo::IsUsing())
            return;

        if (!justPressed)
            return;

        const RawInputSnapshot &input = mWindow->RawInput();
        const float px = static_cast<float>(input.mouseX) - static_cast<float>(mSceneViewportRect.offset.x);
        const float py = static_cast<float>(input.mouseY) - static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);
        if (px < 0.0f || py < 0.0f || px >= width || py >= height)
            return;

        const glm::vec2 ndc{ (px / width) * 2.0f - 1.0f, (py / height) * 2.0f - 1.0f };
        const glm::mat4 viewProj = MakeViewProjection(mCamera.Matrix(), mCamera.mLens.mFovYDegrees,
            mCamera.mLens.mNearZ, mCamera.mLens.mFarZ, width / height);
        const Ray ray = MakeCameraRay(mCamera.Matrix(), viewProj, ndc);

        Entity hitEntity = kNullEntity;
        float hitDistance = std::numeric_limits<float>::max();

        world.GetOrCreateQuery<const WorldTransform, const MeshRenderer, const MeshBounds>().ForEach(
            [&](Entity entity, const WorldTransform &transform, const MeshRenderer &, const MeshBounds &bounds)
            {
                const glm::mat4 worldMatrix = transform.Matrix();
                const glm::mat4 invWorld = glm::inverse(worldMatrix);
                const glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(ray.origin, 1.0f));
                const glm::vec3 localDir = glm::vec3(invWorld * glm::vec4(ray.direction, 0.0f));

                const std::optional<float> t =
                    RayIntersectsAABB(Ray{ localOrigin, localDir }, bounds.mLocalMin, bounds.mLocalMax);
                if (!t)
                    return;

                const glm::vec3 localHit = localOrigin + localDir * (*t);
                const glm::vec3 worldHit = glm::vec3(worldMatrix * glm::vec4(localHit, 1.0f));
                const float distance = glm::length(worldHit - ray.origin);
                if (distance < hitDistance)
                {
                    hitDistance = distance;
                    hitEntity = entity;
                }
            });

        ApplyClick(world.Resource<Selection>(), hitEntity, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
    }

    void Editor::DrawSelectionHighlights(const EngineModeState &mode, World &world)
    {
        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const Selection *selection = world.TryResource<Selection>();
        if (selection == nullptr || selection->mSelected.empty())
            return;

        const float x0 = static_cast<float>(mSceneViewportRect.offset.x);
        const float y0 = static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);

        const glm::mat4 viewProj = MakeViewProjection(mCamera.Matrix(), mCamera.mLens.mFovYDegrees,
            mCamera.mLens.mNearZ, mCamera.mLens.mFarZ, width / height);

        ImDrawList *drawList = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
        drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
        const Entity primary = selection->Primary();

        for (const Entity entity : selection->mSelected)
        {
            if (!world.IsAlive(entity))
                continue;

            const MeshBounds *bounds = world.GetComponent<MeshBounds>(entity);
            if (bounds == nullptr)
                continue;

            const glm::mat4 worldMatrix = ResolveWorld(world, entity);
            const glm::vec3 mn = bounds->mLocalMin;
            const glm::vec3 mx = bounds->mLocalMax;
            const glm::vec3 corners[8] = {
                { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
                { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z } };

            ImVec2 screen[8];
            bool behindCamera = false;
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec4 clip = viewProj * worldMatrix * glm::vec4(corners[i], 1.0f);
                if (clip.w <= 0.0f)
                {
                    behindCamera = true;
                    break;
                }
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                screen[i] = ImVec2(x0 + (ndc.x * 0.5f + 0.5f) * width, y0 + (ndc.y * 0.5f + 0.5f) * height);
            }
            if (behindCamera)
                continue;

            const ImU32 color = (entity == primary) ? IM_COL32(255, 170, 0, 255) : IM_COL32(255, 220, 130, 190);

            static constexpr int kEdges[12][2] = {
                { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
                { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (const auto &edge : kEdges)
                drawList->AddLine(screen[edge[0]], screen[edge[1]], color, 2.0f);
        }

        drawList->PopClipRect();
    }

    void Editor::DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects)
    {
        constexpr const char *kPopupId = "New Project";

        // outside the menu: OpenPopup needs the same ID stack as BeginPopupModal
        if (mNewProjectRequested)
        {
            mNewProjectName[0] = '\0';
            mNewProjectRayTracing = false;
            ImGui::OpenPopup(kPopupId);
            mNewProjectRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPopupId, nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::InputTextWithHint("Name", "Project name...", mNewProjectName, sizeof(mNewProjectName));
        ImGui::Checkbox("Ray Tracing", &mNewProjectRayTracing);
        ImGui::Spacing();

        const std::string_view name(mNewProjectName);
        const bool collides = std::any_of(projects.begin(), projects.end(),
            [&](const ProjectMenuEntry &project)
            { return project.mDir == "games/" + std::string(name); });
        const bool canCreate = !name.empty() && !collides;

        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create"))
        {
            actions.mCreateProjectName = mNewProjectName;
            actions.mCreateProjectRayTracing = mNewProjectRayTracing;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();

        if (collides)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "A project named \"%s\" already exists.",
                                mNewProjectName);
        }

        ImGui::EndPopup();
    }

    EditorActions Editor::DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                                     const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                                     std::string_view currentProject, World &world)
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

            if (ImGui::Begin("Hierarchy"))
                mHierarchyPanel.Draw(world);
            ImGui::End();

            if (ImGui::Begin("Inspector"))
                mInspectorPanel.Draw(world);
            ImGui::End();

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

            UpdateViewportPicking(mode, world);
            DrawSelectionHighlights(mode, world);
            DrawViewportGizmo(mode, world);

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
                    if (ImGui::MenuItem("New Project..."))
                        mNewProjectRequested = true;
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
            DrawNewProjectModal(actions, projects);

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
