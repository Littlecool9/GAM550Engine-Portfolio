/**
 * @file UIBackendImGui.cpp
 * @author Quy Dong Ta
 * @brief Replays a UIDrawList into ImGui's background draw list, and keeps
 *        the game UI's textures as ImGui 1.92 user textures. The only file
 *        in gameui that includes ImGui.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "UIBackend.h"

#include <core/log/Log.h>

#include <imgui.h>
// RegisterUserTexture and UnregisterUserTexture
#include <imgui_internal.h>

#include <cstring>

namespace mir::detail
{
    struct UIBackendTexture
    {
        ImTextureData mData;
        ImGuiContext *mContext = nullptr;
    };

    void UIBackendTextureDeleter::operator()(UIBackendTexture *texture) const
    {
        if (texture == nullptr)
            return;

        if (ImGui::GetCurrentContext() != texture->mContext)
        {
            delete texture;
            return;
        }

        ImTextureData &data = texture->mData;
        if (data.TexID != ImTextureID_Invalid || data.BackendUserData != nullptr)
        {
            MIR_LOG_ERROR("gameui: a UI texture outlived by its renderer backend is leaked; "
                          "destroy UITextures after ImGuiRuntime::Shutdown");
            return;
        }

        ImGui::UnregisterUserTexture(&data);

        ImGui::GetPlatformIO().Textures.find_erase(&data);
        delete texture;
    }

    UIBackendTexturePtr CreateBackendTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            MIR_LOG_ERROR("gameui: a UI texture needs an ImGui context");
            return {};
        }
        if (width == 0 || height == 0 || rgba.size() != static_cast<size_t>(width) * height * 4)
        {
            MIR_LOG_ERROR("gameui: a {}x{} UI texture needs {} bytes of RGBA, got {}", width, height,
                          static_cast<size_t>(width) * height * 4, rgba.size());
            return {};
        }

        UIBackendTexturePtr texture(new UIBackendTexture);
        ImTextureData &data = texture->mData;
        data.Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
        std::memcpy(data.GetPixels(), rgba.data(), rgba.size());
        data.UseColors = true;
        data.UsedRect = ImTextureRect{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};

        ImGui::RegisterUserTexture(&data);
        texture->mContext = ImGui::GetCurrentContext();
        return texture;
    }

    namespace
    {
        ImU32 ToImColor(UIColor color) { return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a)); }

        ImVec2 Min(const UIRect &rect, const UIRect &area) { return ImVec2(area.x + rect.x, area.y + rect.y); }
        ImVec2 Max(const UIRect &rect, const UIRect &area) { return ImVec2(area.x + rect.Right(), area.y + rect.Bottom()); }
    }


    void SubmitToBackground(const UIRect &area, const UIDrawList &list, const UITextures &textures)
    {
        ImDrawList *drawList = ImGui::GetBackgroundDrawList();

        for (const UIDrawCommand &command : list.Commands())
        {

            drawList->PushClipRect(Min(command.mClip, area), Max(command.mClip, area), false);

            switch (command.mKind)
            {
            case UIDrawCommandKind::Rect:
                drawList->AddRectFilled(Min(command.mRect, area), Max(command.mRect, area), ToImColor(command.mColor),
                                        command.mRounding);
                break;

            case UIDrawCommandKind::Texture:
            {
                UIBackendTexture *texture = textures.Find(command.mTexture);
                if (texture == nullptr)
                {
                    // a null texture, or one from another UIContext
                    drawList->AddRectFilled(Min(command.mRect, area), Max(command.mRect, area),
                                            ToImColor(command.mColor));
                    break;
                }

                const UIRect &uv = command.mUV;
                drawList->AddImage(texture->mData.GetTexRef(), Min(command.mRect, area), Max(command.mRect, area),
                                   ImVec2(uv.x, uv.y), ImVec2(uv.Right(), uv.Bottom()), ToImColor(command.mColor));
                break;
            }

            case UIDrawCommandKind::Text:
            {
                const std::string_view text = list.TextOf(command);
                const float fontSize = command.mFontSize > 0.0f ? command.mFontSize : ImGui::GetFontSize();
                drawList->AddText(ImGui::GetFont(), fontSize, Min(command.mRect, area), ToImColor(command.mColor),
                                  text.data(), text.data() + text.size());
                break;
            }
            }

            drawList->PopClipRect();
        }
    }
} // namespace mir::detail
