/**
 * @file PreferencesPanel.cpp
 * @author Jackey Cen
 * @brief The "Preferences" window - per-user editor preferences.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/PreferencesPanel.h>

#include <editor/BackgroundImage.h>

#include <imgui.h>

namespace mir
{
    PreferencesRequest PreferencesPanel::Draw(EditorPreferences &preferences, bool hasBackgroundImage)
    {
        PreferencesRequest request;

        ImGui::SeparatorText("Background Image");
        ImGui::TextDisabled("Saved for this user only, not in the project.");

        request.mSave |= ImGui::Checkbox("Show background image", &preferences.backgroundEnabled);

        ImGui::Text("Image: %s", hasBackgroundImage ? preferences.backgroundFile.c_str() : "(none)");

        ImGui::BeginDisabled(!kCanPickImageFile);
        request.mPickImage = ImGui::Button("Browse...");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasBackgroundImage);
        request.mClearImage = ImGui::Button("Clear");
        ImGui::EndDisabled();

        // applies live while dragging; saved once on release
        ImGui::SliderFloat("Strength", &preferences.backgroundStrength, 0.05f, 0.6f, "%.2f",
            ImGuiSliderFlags_AlwaysClamp);
        request.mSave |= ImGui::IsItemDeactivatedAfterEdit();

        const char *fitNames[] = {"Cover", "Contain", "Stretch"};
        int fit = static_cast<int>(preferences.backgroundFit);
        if (ImGui::Combo("Fit", &fit, fitNames, IM_ARRAYSIZE(fitNames)))
        {
            preferences.backgroundFit = static_cast<BackgroundFit>(fit);
            request.mSave = true;
        }

        if (!mStatusMessage.empty())
            ImGui::TextDisabled("%s", mStatusMessage.c_str());

        return request;
    }
}
