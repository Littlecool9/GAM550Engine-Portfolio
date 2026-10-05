/**
 * @file LogPanel.cpp
 * @author Rahul Nair
 * @brief Renders core::LogHistory - the editor's "Output" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/LogPanel.h>

#include <core/log/LogHistory.h>
#include <editortheme/EditorTheme.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    namespace
    {
        const char *LevelLabel(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace:
                return "Trace";
            case LogLevel::Debug:
                return "Debug";
            case LogLevel::Info:
                return "Info";
            case LogLevel::Warn:
                return "Warn";
            case LogLevel::Error:
                return "Error";
            case LogLevel::Critical:
                return "Critical";
            case LogLevel::Off:
                return "Off";
            }
            return "";
        }

        StatusColor LevelStatusColor(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace:
                return StatusColor::Muted;
            case LogLevel::Debug:
                return StatusColor::Info;
            case LogLevel::Warn:
                return StatusColor::Warning;
            case LogLevel::Error:
            case LogLevel::Critical:
                return StatusColor::Danger;
            default:
                return StatusColor::Muted;
            }
        }

        bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
                return true;
            const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                         [](unsigned char a, unsigned char b)
                                         { return std::tolower(a) == std::tolower(b); });
            return it != haystack.end();
        }
    }

    void LogPanel::Draw()
    {
        if (ImGui::Button("Clear"))
        {
            LogHistory::Clear();
            mSelectionAnchor = -1;
            mSelectionEnd = -1;
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &mAutoScroll);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        int minLevel = static_cast<int>(mMinLevel);
        if (ImGui::Combo(
                "##MinLevel", &minLevel,
                [](void *, int idx) -> const char *
                { return LevelLabel(static_cast<LogLevel>(idx)); },
                nullptr, static_cast<int>(LogLevel::Critical) + 1))
        {
            mMinLevel = static_cast<LogLevel>(minLevel);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##Search", "Search...", mSearchBuf, sizeof(mSearchBuf));

        const std::vector<LogEntry> entries = LogHistory::Snapshot();

        std::vector<std::size_t> visible;
        visible.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].level < mMinLevel)
                continue;
            if (!ContainsCaseInsensitive(entries[i].message, mSearchBuf))
                continue;
            visible.push_back(i);
        }

        ImGui::SameLine();
        if (ImGui::Button("Copy All"))
        {
            std::string clip;
            for (std::size_t index : visible)
            {
                clip += entries[index].message;
                clip += '\n';
            }
            ImGui::SetClipboardText(clip.c_str());
        }
        ImGui::SetItemTooltip("Copy every line currently shown below");

        ImGui::Separator();

        ImGui::BeginChild("##LogScroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const LogEntry &entry = entries[visible[static_cast<std::size_t>(row)]];

                const bool rowSelected = mSelectionAnchor >= 0 &&
                                          row >= std::min(mSelectionAnchor, mSelectionEnd) &&
                                          row <= std::max(mSelectionAnchor, mSelectionEnd);

                ImGui::PushID(row);
                const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", rowSelected, ImGuiSelectableFlags_None,
                                       ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeightWithSpacing())))
                {
                    if (ImGui::GetIO().KeyShift && mSelectionAnchor >= 0)
                        mSelectionEnd = row;
                    else
                    {
                        mSelectionAnchor = row;
                        mSelectionEnd = row;
                    }
                }
                ImGui::SetCursorScreenPos(rowPos);

                if (entry.level == LogLevel::Critical)
                {
                    const ImVec2 rowMax(rowPos.x + ImGui::GetContentRegionAvail().x,
                                         rowPos.y + ImGui::GetTextLineHeightWithSpacing());
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        rowPos, rowMax,
                        ImGui::ColorConvertFloat4ToU32(EditorTheme::Color(StatusColor::Danger, 0.15f)));
                }

                if (entry.level == LogLevel::Info)
                {
                    ImGui::TextUnformatted(entry.message.c_str());
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(LevelStatusColor(entry.level)));
                    ImGui::TextUnformatted(entry.message.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
        }
        clipper.End();

        if (mSelectionAnchor >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
        {
            const int first = std::min(mSelectionAnchor, mSelectionEnd);
            const int last = std::max(mSelectionAnchor, mSelectionEnd);

            std::string clip;
            for (int row = first; row <= last && row < static_cast<int>(visible.size()); ++row)
            {
                clip += entries[visible[static_cast<std::size_t>(row)]].message;
                clip += '\n';
            }
            ImGui::SetClipboardText(clip.c_str());
        }

        if (mAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
    }
}
