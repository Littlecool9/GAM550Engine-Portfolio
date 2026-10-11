/**
 * @file PreferencesPanel.h
 * @author Jackey Cen
 * @brief The "Preferences" window - per-user editor preferences.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/EditorPreferences.h>

#include <string>
#include <utility>

namespace mir
{
    // the panel edits in place; file and texture work is the editor's
    struct PreferencesRequest
    {
        bool mSave = false;
        bool mPickImage = false;
        bool mClearImage = false;
    };

    class PreferencesPanel
    {
    public:
        PreferencesRequest Draw(EditorPreferences &preferences, bool hasBackgroundImage);

        void SetStatus(std::string message) { mStatusMessage = std::move(message); }

    private:
        std::string mStatusMessage;
    };
}
