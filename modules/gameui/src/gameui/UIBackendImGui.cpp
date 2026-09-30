/**
 * @file UIBackendImGui.cpp
 * @author Quy Dong Ta
 * @brief Replays a UIDrawList into ImGui's background draw list. The only file
 *        in gameui that includes ImGui.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "UIBackend.h"

#include <core/log/Log.h>

#include <imgui.h>

namespace mir::detail
{
    namespace
    {
        ImU32 ToImColor(UIColor color) { return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a)); }

        ImVec2 Min(const UIRect &rect, const UIRect &area) { return ImVec2(area.x + rect.x, area.y + rect.y); }
        ImVec2 Max(const UIRect &rect, const UIRect &area) { return ImVec2(area.x + rect.Right(), area.y + rect.Bottom()); }
    }


    void SubmitToBackground(const UIRect &area, const UIDrawList &list)
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
                static bool sWarned = false;
                if (!sWarned)
                {
                    MIR_LOG_WARN("gameui: textures draw as a flat tint until the renderer exposes them to ImGui");
                    sWarned = true;
                }
                drawList->AddRectFilled(Min(command.mRect, area), Max(command.mRect, area), ToImColor(command.mColor));
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
