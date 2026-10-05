/**
 * @file LogPanel.h
 * @author Rahul Nair
 * @brief Renders core::LogHistory - the editor's "Output" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/log/Log.h>

namespace mir
{
    class LogPanel
    {
    public:
        void Draw();

    private:
        char mSearchBuf[128] = {};
        LogLevel mMinLevel = LogLevel::Trace;
        bool mAutoScroll = true;
        int mSelectionAnchor = -1;
        int mSelectionEnd = -1;
    };
}
