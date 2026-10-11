/**
 * @file ProjectSettingsPanel.h
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/IdeSettings.h>
#include <renderer/RenderSettings.h>

#include <filesystem>
#include <string>

namespace mir
{
    class ProjectSettingsPanel
    {
    public:
        void Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath, bool &showInputSettings);

        void OnHidden()
        {
            mDraftLoaded = false;
            mRenderingStatusMessage.clear();
            mIdeDraftLoaded = false;
            mIdeStatusMessage.clear();
        }

    private:
        void DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
            const std::filesystem::path &savePath);
        void DrawExternalTools(const std::filesystem::path &savePath);

        std::string mRenderingStatusMessage;
        RenderSettings mMessageSnapshot;

        bool mDraftLoaded = false;
        RenderSettings mDraftRenderSettings;

        std::string mIdeStatusMessage;
        IdeSettings mIdeMessageSnapshot;
        bool mIdeDraftLoaded = false;
        IdeSettings mDraftIdeSettings;
        char mIdePathBuffer[512]{};
    };
}
