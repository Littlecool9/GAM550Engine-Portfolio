/**
 * @file UIDrawList.cpp
 * @author Quy Dong Ta
 * @brief Records rects, textures and text under a clip stack, in coordinates
 *        local to the UI area, for the backend to replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <core/log/Assert.h>

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

    void UIDrawList::Texture(const UIRect &rect, TextureHandle texture, UIColor tint)
    {
        const UIRect clip = CurrentClip();
        if (Intersect(rect, clip).IsEmpty())
            return;

        mCommands.push_back(UIDrawCommand{
                                    .mKind = UIDrawCommandKind::Texture, 
                                    .mRect = rect, 
                                    .mClip = clip, 
                                    .mColor = tint, 
                                    .mTexture = texture});
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
