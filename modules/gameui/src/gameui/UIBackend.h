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
#include <gameui/UITextures.h>
#include <gameui/UITypes.h>

#include <cstdint>
#include <memory>
#include <span>

namespace mir::detail
{
    using UIBackendTexturePtr = std::unique_ptr<UIBackendTexture, UIBackendTextureDeleter>;

    UIBackendTexturePtr CreateBackendTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba);

    void SubmitToBackground(const UIRect &area, const UIDrawList &list, const UITextures &textures);
}
