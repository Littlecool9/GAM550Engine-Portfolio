/**
 * @file ProjectSettingsPanel.cpp
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk,
 *        plus an entry point into the separate Input Settings panel
 *        (gh-155).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/ProjectSettingsPanel.h>

#include <imgui.h>

namespace mir
{
    void ProjectSettingsPanel::DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
        const std::filesystem::path &savePath)
    {
        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &draftRenderSettings.rayTracing);
        ImGui::Checkbox("Distance Fog", &draftRenderSettings.fog);

        if (ImGui::Button("Save##Rendering"))
        {
            renderSettings = draftRenderSettings;
            mRenderingStatusMessage = renderSettings.SaveFile(savePath) ? "Saved." : "Save failed - see log.";
        }

        if (!mRenderingStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mRenderingStatusMessage.c_str());
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
    }
}
