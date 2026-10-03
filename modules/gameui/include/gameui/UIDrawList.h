/**
 * @file UIDrawList.h
 * @author Quy Dong Ta
 * @brief Records rects, textures, nine-sliced sprites and text under a clip
 *        stack, in coordinates local to the UI area, for the backend to
 *        replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

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

    struct UIDrawCommand
    {
        UIDrawCommandKind mKind = UIDrawCommandKind::Rect;

        UIRect mRect;

        UIRect mClip;

        UIColor mColor;


        float mRounding = 0.0f;


        float mFontSize = 0.0f;


        UITexture mTexture;
        UIRect mUV{0.0f, 0.0f, 1.0f, 1.0f};

        uint32_t mTextOffset = 0;
        uint32_t mTextLength = 0;
    };

    class UIDrawList
    {
    public:

        void Reset(float width, float height);

        void Rect(const UIRect &rect, UIColor color, float rounding = 0.0f);
        void Texture(const UIRect &rect, UITexture texture, UIColor tint = kUIWhite,
                     const UIRect &uv = UIRect{0.0f, 0.0f, 1.0f, 1.0f});

        void Sprite(const UIRect &rect, const UISprite &sprite, UIColor tint = kUIWhite);
        void Text(float x, float y, std::string_view text, UIColor color, float fontSize = 0.0f);

        void PushClip(const UIRect &rect);
        void PopClip();

        UIRect CurrentClip() const;

        size_t ClipDepth() const { return mClipStack.empty() ? 0 : mClipStack.size() - 1; }

        std::span<const UIDrawCommand> Commands() const { return mCommands; }
        std::string_view TextOf(const UIDrawCommand &command) const;

    private:
        std::vector<UIDrawCommand> mCommands;

        std::vector<UIRect> mClipStack;

        std::string mTextBuffer;
    };
} // namespace mir
