/**
 * @file UIDrawList.cpp
 * @author Quy Dong Ta
 * @brief Records rects, textures, nine-sliced sprites and text under a clip
 *        stack, in coordinates local to the UI area, for the backend to
 *        replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <core/log/Assert.h>

#include <algorithm>

namespace mir
{
    void UIDrawList::Reset(float width, float height)
    {
        mCommands.clear();
        mTextBuffer.clear();
        mClipStack.clear();
        mClipStack.push_back(UIRect{0.0f, 0.0f, std::max(width, 0.0f), std::max(height, 0.0f)});
    }

    void UIDrawList::Rect(const UIRect &rect, UIColor color, float rounding)
    {
        const UIRect clip = CurrentClip();
        if (Intersect(rect, clip).IsEmpty())
            return;

        mCommands.push_back(UIDrawCommand{
                            .mKind = UIDrawCommandKind::Rect, 
                            .mRect = rect, 
                            .mClip = clip, 
                            .mColor = color, 
                            .mRounding = rounding});
    }

    void UIDrawList::Texture(const UIRect &rect, UITexture texture, UIColor tint, const UIRect &uv)
    {
        const UIRect clip = CurrentClip();
        if (Intersect(rect, clip).IsEmpty())
            return;

        mCommands.push_back(UIDrawCommand{
                                    .mKind = UIDrawCommandKind::Texture, 
                                    .mRect = rect, 
                                    .mClip = clip, 
                                    .mColor = tint, 
                                    .mTexture = texture,
                                    .mUV = uv});
    }

    void UIDrawList::Sprite(const UIRect &rect, const UISprite &sprite, UIColor tint)
    {
        if (!sprite.IsValid() || rect.IsEmpty())
            return;

        const UIRect &source = sprite.mSource;


        const float left = std::clamp(sprite.mSlice.mLeft, 0.0f, source.width);
        const float right = std::clamp(sprite.mSlice.mRight, 0.0f, source.width - left);
        const float top = std::clamp(sprite.mSlice.mTop, 0.0f, source.height);
        const float bottom = std::clamp(sprite.mSlice.mBottom, 0.0f, source.height - top);

        float scale = 1.0f;
        if (left + right > rect.width)
            scale = std::min(scale, rect.width / (left + right));
        if (top + bottom > rect.height)
            scale = std::min(scale, rect.height / (top + bottom));

        const float xs[4] = {rect.x, rect.x + left * scale, rect.Right() - right * scale, rect.Right()};
        const float ys[4] = {rect.y, rect.y + top * scale, rect.Bottom() - bottom * scale, rect.Bottom()};
        const float us[4] = {source.x, source.x + left, source.Right() - right, source.Right()};
        const float vs[4] = {source.y, source.y + top, source.Bottom() - bottom, source.Bottom()};

        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const UIRect piece{xs[column], ys[row], xs[column + 1] - xs[column], ys[row + 1] - ys[row]};
                if (piece.IsEmpty())
                    continue;

                const UIRect uv{us[column] / sprite.mTextureWidth, vs[row] / sprite.mTextureHeight,
                                (us[column + 1] - us[column]) / sprite.mTextureWidth,
                                (vs[row + 1] - vs[row]) / sprite.mTextureHeight};
                Texture(piece, sprite.mTexture, tint, uv);
            }
        }
    }

    void UIDrawList::Text(float x, float y, std::string_view text, UIColor color, float fontSize)
    {

        const UIRect clip = CurrentClip();
        if (text.empty() || clip.IsEmpty())
            return;

        const uint32_t offset = static_cast<uint32_t>(mTextBuffer.size());
        mTextBuffer.append(text);

        mCommands.push_back(UIDrawCommand{.mKind = UIDrawCommandKind::Text,
                                          .mRect = UIRect{x, y, 0.0f, 0.0f},
                                          .mClip = clip,
                                          .mColor = color,
                                          .mFontSize = fontSize,
                                          .mTextOffset = offset,
                                          .mTextLength = static_cast<uint32_t>(text.size())});
    }

    void UIDrawList::PushClip(const UIRect &rect) 
    { 
        mClipStack.push_back(Intersect(CurrentClip(), rect)); 
    }

    void UIDrawList::PopClip()
    {
        MIR_ASSERT(mClipStack.size() > 1, "UIDrawList::PopClip without a matching PushClip");
        
        if (mClipStack.size() > 1)
            mClipStack.pop_back();
    }

    UIRect UIDrawList::CurrentClip() const { return mClipStack.empty() ? UIRect{} : mClipStack.back(); }

    std::string_view UIDrawList::TextOf(const UIDrawCommand &command) const
    {
        if (command.mKind != UIDrawCommandKind::Text)
            return {};

        return std::string_view(mTextBuffer).substr(command.mTextOffset, command.mTextLength);
    }
} // namespace mir
