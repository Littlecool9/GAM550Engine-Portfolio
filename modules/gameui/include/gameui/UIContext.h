/**
 * @file UIContext.h
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, and
 *        the draw list that is submitted when the frame ends.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIDrawList.h"
#include "UITypes.h"

#include <input/InputState.h>

namespace mir
{
    class UIContext
    {
    public:
        UIContext() = default;

        UIContext(const UIContext &) = delete;
        UIContext &operator=(const UIContext &) = delete;
        UIContext(UIContext &&) = delete;
        UIContext &operator=(UIContext &&) = delete;

        // area is in window pixels: the editor's scene viewport, or the whole
        // window. Everything drawn until EndFrame is relative to its top-left
        // and clipped to it. Call inside the UI runtime's frame bracket.
        void BeginFrame(const UIRect &area, InputState &input);

        // Submits the frame's draw list over the 3D scene and under every
        // editor panel. Call before the UI runtime's frame ends.
        void EndFrame();

        UIDrawList &DrawList() { return mDrawList; }

        // The area passed to BeginFrame
        const UIRect &Area() const { return mArea; }

        // Only between BeginFrame and EndFrame; null outside a frame
        InputState *Input() const { return mInput; }

        UIStyle &Style() { return mStyle; }
        const UIStyle &Style() const { return mStyle; }

    private:
        UIDrawList mDrawList;
        UIRect mArea;
        InputState *mInput = nullptr;
        UIStyle mStyle;
        bool mInFrame = false;
    };
} // namespace mir
