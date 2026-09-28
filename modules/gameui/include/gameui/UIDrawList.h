/**
 * @file UIDrawList.h
 * @author Quy Dong Ta
 * @brief Records rects, textures and text under a clip stack, in coordinates
 *        local to the UI area, for the backend to replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <renderer/Texture.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class UIDrawCommandKind : uint8_t
    {
        Rect,
        Texture,
        Text,
    };

    // One recorded draw, local to the UI area
    struct UIDrawCommand
    {
        UIDrawCommandKind mKind = UIDrawCommandKind::Rect;

        // Rect, Texture: the area covered. Text: x, y is the top-left of the
        // first line; width and height are unused.
        UIRect mRect;

        // Already the intersection of every clip pushed around this command
        UIRect mClip;

        UIColor mColor;

        // Rect only
        float mRounding = 0.0f;

        // Text only; 0 is the shared font's own size
        float mFontSize = 0.0f;

        // Texture only
        TextureHandle mTexture;

        // Text only: a range of the list's text buffer, read with TextOf
        uint32_t mTextOffset = 0;
        uint32_t mTextLength = 0;
    };

    class UIDrawList
    {
    public:
        // Empties the list and sets the root clip to {0, 0, width, height}
        void Reset(float width, float height);

        void Rect(const UIRect &rect, UIColor color, float rounding = 0.0f);
        void Texture(const UIRect &rect, TextureHandle texture, UIColor tint = kUIWhite);
        void Text(float x, float y, std::string_view text, UIColor color, float fontSize = 0.0f);

        // Narrows the clip to its intersection with rect until the matching
        // PopClip. Popping the root clip is a programmer error.
        void PushClip(const UIRect &rect);
        void PopClip();

        UIRect CurrentClip() const;

        // Pushes above the root clip; 0 when every PushClip has been popped
        size_t ClipDepth() const { return mClipStack.empty() ? 0 : mClipStack.size() - 1; }

        std::span<const UIDrawCommand> Commands() const { return mCommands; }
        std::string_view TextOf(const UIDrawCommand &command) const;

    private:
        std::vector<UIDrawCommand> mCommands;

        // [0] is the root clip from Reset
        std::vector<UIRect> mClipStack;

        // every Text command's characters, back to back
        std::string mTextBuffer;
    };
} // namespace mir
