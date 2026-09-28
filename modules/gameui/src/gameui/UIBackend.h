/**
 * @file UIBackend.h
 * @author Quy Dong Ta
 * @brief The seam between the game UI and whatever draws it. Declares no
 *        backend type, so only the backend's own .cpp includes the backend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIDrawList.h>
#include <gameui/UITypes.h>

namespace mir::detail
{

    void SubmitToBackground(const UIRect &area, const UIDrawList &list);
}
