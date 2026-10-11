/**
 * @file ContentBrowserPanel.cpp
 * @author Rahul Nair
 * @brief Draws the content browser panel and turns clicks into EditorActions content-browser requests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/ContentBrowserPanel.h>

#include <editortheme/EditorTheme.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace mir
{
    namespace
    {
        std::string Lowered(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string ParentOf(const std::string &relativePath)
        {
            const auto slash = relativePath.find_last_of('/');
            return slash == std::string::npos ? std::string{} : relativePath.substr(0, slash);
        }

        const char *IconFor(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return ICON_FA_FILE;
            const std::string extension = Lowered(name.substr(dot));
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg")
                return ICON_FA_FILE_IMAGE;
            if (extension == ".glb" || extension == ".gltf")
                return ICON_FA_CUBE;
            if (extension == ".wav" || extension == ".ogg" || extension == ".mp3")
                return ICON_FA_FILE_AUDIO;
            if (extension == ".json" || extension == ".lua")
                return ICON_FA_FILE_CODE;
            return ICON_FA_FILE;
        }

        bool IsTextFile(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return false;
            const std::string extension = Lowered(name.substr(dot));
            return extension == ".lua" || extension == ".json";
        }

        std::string FormatSize(uint64_t bytes)
        {
            constexpr const char *kUnits[] = { "B", "KB", "MB", "GB", "TB" };
            constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

            double value = static_cast<double>(bytes);
            size_t unit = 0;
            while (value >= 1024.0 && unit + 1 < kUnitCount)
            {
                value /= 1024.0;
                ++unit;
            }

            char text[32]{};
            if (unit == 0)
                std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
            else
                std::snprintf(text, sizeof(text), "%.1f %s", value, kUnits[unit]);
            return text;
        }

        // Built fresh from the flat entry list every Draw - directories are
        // explicit entries, so an empty or just-created folder still shows up.
        struct FolderNode
        {
            std::string mName;
            std::string mRelativePath;
            std::map<std::string, FolderNode> mChildren;
        };

        void InsertFolder(FolderNode &root, const std::string &relativePath)
        {
            FolderNode *node = &root;
            std::string accumulated;
            size_t start = 0;
            while (start <= relativePath.size())
            {
                const size_t slash = relativePath.find('/', start);
                const size_t length = (slash == std::string::npos ? relativePath.size() : slash) - start;
                const std::string segment = relativePath.substr(start, length);
                accumulated = accumulated.empty() ? segment : accumulated + '/' + segment;
                node = &node->mChildren[segment];
                node->mName = segment;
                node->mRelativePath = accumulated;
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }

        void DrawFolderTree(const FolderNode &node, std::string &currentFolder)
        {
            for (const auto &[name, child] : node.mChildren)
            {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (child.mChildren.empty())
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (currentFolder == child.mRelativePath)
                    flags |= ImGuiTreeNodeFlags_Selected;
                const bool open =
                    ImGui::TreeNodeEx(child.mName.c_str(), flags, ICON_FA_FOLDER " %s", child.mName.c_str());
                if (ImGui::IsItemClicked())
                    currentFolder = child.mRelativePath;
                if (open && !child.mChildren.empty())
                {
                    DrawFolderTree(child, currentFolder);
                    ImGui::TreePop();
                }
            }
        }
    } // namespace

    ContentBrowserAction ContentBrowserPanel::Draw(const ContentBrowserView &view)
    {
        ContentBrowserAction action;
        if (!view.mAvailable)
        {
            ImGui::TextWrapped("Content browser is unavailable: %s", view.mUnavailableReason.c_str());
            return action;
        }

        if (!view.mLastRenamedFrom.empty())
        {
            if (mSelectedPath == view.mLastRenamedFrom)
                mSelectedPath = view.mLastRenamedTo;
            if (mRenameTarget == view.mLastRenamedFrom)
                mRenameTarget.clear();
        }

        ImGui::TextDisabled("%s", view.mContentRoot.c_str());
        if (!view.mLastError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
            ImGui::TextWrapped("%s", view.mLastError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        FolderNode root;
        for (const auto &entry : view.mEntries)
            if (entry.mKind == ContentBrowserEntryKind::Directory)
                InsertFolder(root, entry.mRelativePath);

        const float treeWidth = ImGui::GetContentRegionAvail().x * 0.3f;
        if (ImGui::BeginChild("ContentBrowserTree", ImVec2(treeWidth, 0), ImGuiChildFlags_Borders))
        {
            ImGuiTreeNodeFlags rootFlags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (mCurrentFolder.empty())
                rootFlags |= ImGuiTreeNodeFlags_Selected;
            const bool rootOpen = ImGui::TreeNodeEx("##content-root", rootFlags, ICON_FA_FOLDER_TREE " %s",
                view.mContentRoot.empty() ? "Content" : view.mContentRoot.c_str());
            if (ImGui::IsItemClicked())
                mCurrentFolder.clear();
            if (rootOpen)
            {
                DrawFolderTree(root, mCurrentFolder);
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        if (ImGui::BeginChild("ContentBrowserFiles", ImVec2(0, 0), ImGuiChildFlags_Borders))
            DrawEntries(view, action);
        ImGui::EndChild();

        DrawDeleteConfirm(action);
        return action;
    }

    void ContentBrowserPanel::DrawEntries(const ContentBrowserView &view, ContentBrowserAction &action)
    {
        const std::string label = "/" + mCurrentFolder;
        ImGui::TextDisabled("%s", label.c_str());

        if (!mCurrentFolder.empty() && ImGui::SmallButton(ICON_FA_FOLDER_OPEN " .."))
            mCurrentFolder = ParentOf(mCurrentFolder);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER_PLUS " New Folder"))
        {
            mCreatingFolder = true;
            mNewFolderBuffer[0] = '\0';
        }

        if (mCreatingFolder)
        {
            ImGui::SetNextItemWidth(200.0f);
            const bool submitted = ImGui::InputTextWithHint("##new-folder", "folder name", mNewFolderBuffer,
                sizeof(mNewFolderBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                mCreatingFolder = false;
            else if (submitted && mNewFolderBuffer[0] != '\0')
            {
                action.mKind = ContentBrowserActionKind::CreateFolder;
                action.mTargetPath = mCurrentFolder;
                action.mNewName = mNewFolderBuffer;
                mCreatingFolder = false;
            }
        }

        ImGui::Separator();

        if (ImGui::BeginTable(
            "content-entries", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();

            for (const auto &entry : view.mEntries)
            {
                if (ParentOf(entry.mRelativePath) != mCurrentFolder)
                    continue;

                ImGui::PushID(entry.mRelativePath.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                if (mRenameTarget == entry.mRelativePath)
                {
                    ImGui::SetNextItemWidth(-1);
                    const bool submitted = ImGui::InputText("##rename", mRenameBuffer, sizeof(mRenameBuffer),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                        mRenameTarget.clear();
                    else if (submitted && mRenameBuffer[0] != '\0')
                    {
                        action.mKind = ContentBrowserActionKind::Rename;
                        action.mTargetPath = entry.mRelativePath;
                        action.mNewName = mRenameBuffer;
                        mRenameTarget.clear();
                    }
                }
                else
                {
                    const char *icon =
                        entry.mKind == ContentBrowserEntryKind::Directory ? ICON_FA_FOLDER : IconFor(entry.mName);
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                        ImGuiTreeNodeFlags_SpanAllColumns;
                    if (entry.mRelativePath == mSelectedPath)
                        flags |= ImGuiTreeNodeFlags_Selected;
                    ImGui::TreeNodeEx("##entry", flags, "%s  %s", icon, entry.mName.c_str());
                    if (ImGui::IsItemClicked())
                        mSelectedPath = entry.mRelativePath;
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (entry.mKind == ContentBrowserEntryKind::Directory)
                            mCurrentFolder = entry.mRelativePath;
                        else if (IsTextFile(entry.mName))
                        {
                            action.mKind = ContentBrowserActionKind::OpenInIde;
                            action.mTargetPath = entry.mRelativePath;
                        }
                    }

                    if (ImGui::BeginPopupContextItem())
                    {
                        mSelectedPath = entry.mRelativePath;
                        if (ImGui::MenuItem("Rename"))
                        {
                            mRenameTarget = entry.mRelativePath;
                            std::snprintf(mRenameBuffer, sizeof(mRenameBuffer), "%s", entry.mName.c_str());
                        }
                        if (ImGui::MenuItem("Delete"))
                        {
                            mPendingDelete = entry.mRelativePath;
                            mPendingDeleteIsFolder = entry.mKind == ContentBrowserEntryKind::Directory;
                            mDeleteConfirmRequested = true;
                        }
                        ImGui::EndPopup();
                    }
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(entry.mKind == ContentBrowserEntryKind::Directory ? "Folder" : "File");
                ImGui::TableSetColumnIndex(2);
                if (entry.mKind == ContentBrowserEntryKind::File)
                    ImGui::TextUnformatted(FormatSize(entry.mSizeBytes).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void ContentBrowserPanel::DrawDeleteConfirm(ContentBrowserAction &action)
    {
        constexpr const char *kPopupId = "Delete##ContentBrowserConfirm";

        // outside any row's ID stack: OpenPopup needs the same ID stack as BeginPopupModal
        if (mDeleteConfirmRequested)
        {
            ImGui::OpenPopup(kPopupId);
            mDeleteConfirmRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(
            kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::Text("Delete \"%s\"?", mPendingDelete.c_str());
        if (mPendingDeleteIsFolder)
            ImGui::TextDisabled("This folder and everything inside it will be deleted. This cannot be undone.");
        else
            ImGui::TextDisabled("This cannot be undone.");
        ImGui::Spacing();

        if (ImGui::Button("Delete"))
        {
            action.mKind = ContentBrowserActionKind::Delete;
            action.mTargetPath = mPendingDelete;
            mPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            mPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
} // namespace mir
