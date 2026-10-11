/**
 * @file ProjectSettingsPanel.cpp
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/ProjectSettingsPanel.h>

#include <imgui.h>

#include <cstdio>

namespace mir
{
    void ProjectSettingsPanel::DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
        const std::filesystem::path &savePath)
    {
        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &draftRenderSettings.rayTracing);
        ImGui::Checkbox("Distance Fog", &draftRenderSettings.fog);

        if (!mRenderingStatusMessage.empty() && draftRenderSettings != mMessageSnapshot)
            mRenderingStatusMessage.clear();

        if (ImGui::Button("Save##Rendering"))
        {
            if (draftRenderSettings.SaveFile(savePath))
            {
                renderSettings = draftRenderSettings;
                mRenderingStatusMessage = "Saved.";
            }
            else
            {
                mRenderingStatusMessage = "Save failed - see log.";
            }
            mMessageSnapshot = draftRenderSettings;
        }

        if (!mRenderingStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mRenderingStatusMessage.c_str());
        }
    }

    void ProjectSettingsPanel::DrawExternalTools(const std::filesystem::path &savePath)
    {
        if (!mIdeDraftLoaded)
        {
            mDraftIdeSettings = IdeSettings::LoadFile(savePath).value_or(IdeSettings{});
            std::snprintf(mIdePathBuffer, sizeof(mIdePathBuffer), "%s", mDraftIdeSettings.path.c_str());
            std::snprintf(mIdeArgsBuffer, sizeof(mIdeArgsBuffer), "%s", mDraftIdeSettings.argsTemplate.c_str());
            mIdeDraftLoaded = true;
        }

        ImGui::SeparatorText("External Tools");
        ImGui::TextWrapped(
            "Double-clicking a .lua or .json file in the Content Browser opens the whole project folder as the "
            "workspace root in a new IDE window, with the file open. Empty IDE path auto-detects VS Code. Launch "
            "args: {project} and {file} are substituted; a non-VS Code tool may need a different template.");

        if (ImGui::InputTextWithHint("IDE path", "auto-detect (VS Code)", mIdePathBuffer, sizeof(mIdePathBuffer)))
            mDraftIdeSettings.path = mIdePathBuffer;

        if (ImGui::InputTextWithHint("Launch args", kDefaultIdeArgsTemplate, mIdeArgsBuffer, sizeof(mIdeArgsBuffer)))
            mDraftIdeSettings.argsTemplate = mIdeArgsBuffer;

        if (!mIdeStatusMessage.empty() && mDraftIdeSettings != mIdeMessageSnapshot)
            mIdeStatusMessage.clear();

        if (ImGui::Button("Detect VS Code"))
        {
            if (const std::optional<std::filesystem::path> detected = DetectDefaultIde())
            {
                mDraftIdeSettings.path = detected->string();
                std::snprintf(mIdePathBuffer, sizeof(mIdePathBuffer), "%s", mDraftIdeSettings.path.c_str());
            }
            else
                mIdeStatusMessage = "No VS Code installation found - set a path manually.";
            mIdeMessageSnapshot = mDraftIdeSettings;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save##Ide"))
        {
            mIdeStatusMessage = mDraftIdeSettings.SaveFile(savePath) ? "Saved." : "Save failed - see log.";
            mIdeMessageSnapshot = mDraftIdeSettings;
        }

        if (!mIdeStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mIdeStatusMessage.c_str());
        }
    }

    void ProjectSettingsPanel::Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath,
        bool &showInputSettings)
    {
        if (!mDraftLoaded)
        {
            mDraftRenderSettings = renderSettings;
            mDraftLoaded = true;
        }

        ImGui::SeparatorText("Input");
        ImGui::TextWrapped("Actions and bindings live in their own panel.");
        if (ImGui::Button("Open Input Settings"))
            showInputSettings = true;

        DrawRendering(renderSettings, mDraftRenderSettings, savePath);
        DrawExternalTools(savePath);
    }
}
