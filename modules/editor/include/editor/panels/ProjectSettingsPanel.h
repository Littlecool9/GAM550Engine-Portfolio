/**
 * @file ProjectSettingsPanel.h
 * @author Rahul Nair
 * @brief The "Project Settings" window - Input section (add/remove actions,
 *        add/remove/rebind bindings) and Rendering section, each saved to
 *        disk independently.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputMap.h>
#include <renderer/RenderSettings.h>
#include <window/InputSnapshot.h>

#include <filesystem>
#include <string>

namespace mir
{
    class ProjectSettingsPanel
    {
    public:
        void Draw(InputMap &inputMap, RenderSettings &renderSettings, const RawInputSnapshot &raw,
                   const std::filesystem::path &savePath);

        void CancelListening() { mListeningAction = mListeningBinding = -1; }

    private:
        bool DrawAction(InputAction &action, int actionIndex, const RawInputSnapshot &raw);

        bool DrawBinding(const InputAction &action, InputBinding &binding, int actionIndex, int bindingIndex,
                          const RawInputSnapshot &raw);

        bool TryCaptureBinding(InputBinding &binding, const RawInputSnapshot &raw);

        void DrawRendering(RenderSettings &renderSettings, const std::filesystem::path &savePath);

        char mNewActionName[64] = {};
        int mNewActionType = 0;

        int mListeningAction = -1;
        int mListeningBinding = -1;
        RawInputSnapshot mPreviousRaw;

        std::string mStatusMessage;
        std::string mRenderingStatusMessage;
    };
}
