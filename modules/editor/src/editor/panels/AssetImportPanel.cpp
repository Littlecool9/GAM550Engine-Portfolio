#include <editor/panels/AssetImportPanel.h>

#include <editortheme/EditorTheme.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace mir
{
    namespace
    {
        const char *StatusName(AssetImportStatus status)
        {
            switch (status)
            {
            case AssetImportStatus::Ready:
                return "Ready";
            case AssetImportStatus::Queued:
                return "Queued";
            case AssetImportStatus::Processing:
                return "Processing";
            case AssetImportStatus::UpToDate:
                return "Up to date";
            case AssetImportStatus::Failed:
                return "Failed";
            case AssetImportStatus::WaitingForStop:
                return "Waiting for Stop";
            case AssetImportStatus::Applied:
                return "Applied";
            case AssetImportStatus::Missing:
                return "Missing";
            case AssetImportStatus::Canceled:
                return "Canceled";
            }
            return "Unknown";
        }

        StatusColor StatusTone(AssetImportStatus status)
        {
            switch (status)
            {
            case AssetImportStatus::Failed:
                return StatusColor::Danger;
            case AssetImportStatus::Missing:
            case AssetImportStatus::Canceled:
            case AssetImportStatus::WaitingForStop:
                return StatusColor::Warning;
            case AssetImportStatus::Queued:
            case AssetImportStatus::Processing:
                return StatusColor::Info;
            default:
                return StatusColor::Muted;
            }
        }

        struct KindRow
        {
            const char *mKind;
            const char *mHeading;
        };
        // Fixed order, so a package always reads the same way.
        constexpr KindRow kKinds[]{{"model", "Model"},        {"scene", "Scenes"},     {"animation", "Animations"},
                                   {"material", "Materials"}, {"texture", "Textures"}, {"mesh", "Meshes"},
                                   {"skeleton", "Skeletons"}};

        std::string Lowered(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string FileName(const std::string &path)
        {
            const auto slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string ParentDirectory(const std::string &path)
        {
            const auto slash = path.find_last_of("/\\");
            return slash == std::string::npos ? std::string{} : path.substr(0, slash);
        }

        // Determine handling a directory or a package
        bool LooksLikeDirectory(const std::string &path) { return FileName(path).find('.') == std::string::npos; }

        bool LooksLikePackage(const std::string &path)
        {
            const std::string name = FileName(path);
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return true;
            const std::string extension = Lowered(name.substr(dot));
            return extension == ".gltf" || extension == ".glb";
        }

        bool Contains(const std::string &haystack, const char *needle)
        {
            return needle[0] == '\0' || Lowered(haystack).find(Lowered(needle)) != std::string::npos;
        }

        bool Matches(const AssetImportModelView &model, const char *filter)
        {
            if (Contains(model.mPath, filter))
                return true;
            for (const auto &entry : model.mContents)
                if (Contains(entry.mName, filter))
                    return true;
            return false;
        }

        std::string HexId(uint64_t id)
        {
            char text[17]{};
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id));
            return text;
        }

        bool EditString(const char *label, const char *hint, std::string &value, size_t capacity)
        {
            std::vector<char> buffer(capacity, '\0');
            std::snprintf(buffer.data(), capacity, "%s", value.c_str());
            const bool changed = hint != nullptr ? ImGui::InputTextWithHint(label, hint, buffer.data(), capacity)
                                                 : ImGui::InputText(label, buffer.data(), capacity);
            if (changed)
                value = buffer.data();
            return changed;
        }

        void DrawStatus(const AssetImportModelView &model)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusTone(model.mStatus)));
            ImGui::TextUnformatted(StatusName(model.mStatus));
            ImGui::PopStyleColor();
            if (!ImGui::IsItemHovered())
                return;
            if (!model.mDiagnostic.empty())
                ImGui::SetTooltip("%s", model.mDiagnostic.c_str());
            else if (model.mLastSuccess != 0)
            {
                const std::time_t when = static_cast<std::time_t>(model.mLastSuccess);
                std::tm parts{};
#ifdef _WIN32
                localtime_s(&parts, &when);
#else
                localtime_r(&when, &parts);
#endif
                char text[64]{};
                std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
                ImGui::SetTooltip("Last imported %s", text);
            }
        }
    } // namespace

    void AssetImportPanel::AcceptDrops(std::span<const std::string> dropped)
    {
        if (dropped.empty())
            return;
        mDropError.clear();
        for (const auto &path : dropped)
        {
            if (!LooksLikePackage(path))
            {
                mDropError = FileName(path) + ": not a .gltf / .glb file or a folder holding one";
                continue;
            }
            if (std::any_of(mStaged.begin(), mStaged.end(),
                            [&](const Staged &staged) { return staged.mSource == path; }))
                continue;

            mStaged.push_back({path, LooksLikeDirectory(path) ? path : ParentDirectory(path), std::string{}});
        }
    }

    AssetImportAction AssetImportPanel::Draw(const AssetImportView &view, std::span<const std::string> dropped)
    {
        AssetImportAction action;
        if (!view.mAvailable)
        {
            ImGui::TextWrapped("Asset importing is unavailable: %s", view.mUnavailableReason.c_str());
            ImGui::InputText("Source project root", mWorkspaceRoot, sizeof(mWorkspaceRoot));
            if (mWorkspaceRoot[0] != '\0' && ImGui::Button("Bind source project"))
            {
                action.mKind = AssetImportActionKind::BindWorkspace;
                action.mWorkspaceRoot = mWorkspaceRoot;
            }
            return action;
        }

        AcceptDrops(dropped);

        const char *hint = "Drop a .gltf / .glb file - or the folder that holds one - on this panel";
        const float zoneHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        if (ImGui::BeginChild("drop-zone", ImVec2(0, zoneHeight), ImGuiChildFlags_Borders))
        {
            const float offset = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x) * 0.5f;
            if (offset > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            ImGui::TextDisabled("%s", hint);
        }
        ImGui::EndChild();

        if (!mDropError.empty() || !view.mLastError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
            if (!mDropError.empty())
                ImGui::TextWrapped("%s", mDropError.c_str());
            if (!view.mLastError.empty())
                ImGui::TextWrapped("%s", view.mLastError.c_str());
            ImGui::PopStyleColor();
            if (!mDropError.empty() && ImGui::SmallButton("Dismiss"))
                mDropError.clear();
        }

        if (!mStaged.empty())
        {
            ImGui::SeparatorText("Dropped packages");
            int submit = -1;
            int discard = -1;
            for (int i = 0; i < static_cast<int>(mStaged.size()); ++i)
            {
                Staged &staged = mStaged[i];
                ImGui::PushID(i);
                ImGui::TextWrapped("%s", staged.mSource.c_str());
                EditString("Package root", "folder holding the .bin and textures", staged.mPackageRoot, 512);
                EditString("Target", "models/<name> - empty picks one for you", staged.mTarget, 256);
                ImGui::BeginDisabled(view.mProcessRunning);
                if (ImGui::Button("Import"))
                    submit = i;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Discard"))
                    discard = i;
                ImGui::PopID();
                ImGui::Separator();
            }
            if (mStaged.size() > 1)
            {
                ImGui::BeginDisabled(view.mProcessRunning);
                if (ImGui::Button("Import all"))
                    mImportAll = true;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Discard all"))
                {
                    mStaged.clear();
                    mImportAll = false;
                }
            }
            if (submit < 0 && discard < 0 && mImportAll && !view.mProcessRunning && !mStaged.empty())
                submit = 0;
            if (discard >= 0)
                mStaged.erase(mStaged.begin() + discard);
            else if (submit >= 0)
            {
                action.mKind = AssetImportActionKind::Import;
                action.mExternalSource = mStaged[submit].mSource;
                action.mPackageRoot = mStaged[submit].mPackageRoot;
                action.mTargetDirectory = mStaged[submit].mTarget;
                mStaged.erase(mStaged.begin() + submit);
            }
            if (mStaged.empty())
                mImportAll = false;
        }

        if (view.mProcessRunning)
        {
            ImGui::TextDisabled("AssetCooker is running");
            ImGui::SameLine();
            if (ImGui::Button("Cancel current batch"))
                action.mKind = AssetImportActionKind::Cancel;
        }

        bool automatic = view.mAutoImport;
        if (ImGui::Checkbox("Watch source assets and re-import on change", &automatic))
        {
            action.mKind = AssetImportActionKind::SetAutoImport;
            action.mEnabled = automatic;
        }
        ImGui::SetItemTooltip("Polls %s\n"
                              "- the source project, not the copy next to the executable - for changes to\n"
                              ".gltf, .glb, .bin, .png and .jpg files, and re-imports a package half a\n"
                              "second after it stops changing.\n"
                              "When off, changes are still tracked but nothing is cooked until you press\n"
                              "Reimport on a row.",
                              view.mWatchedRoot.empty() ? "the source asset directory" : view.mWatchedRoot.c_str());

        ImGui::SeparatorText("Managed models");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("##filter", "Filter by model path or content name", mFilter, sizeof(mFilter));

        std::string scrollTo;
        if (!view.mLastImported.empty() && view.mLastImported != mScrolledTo)
        {
            mSelectedPath = view.mLastImported;
            mScrolledTo = view.mLastImported;
            scrollTo = view.mLastImported;
        }

        if (ImGui::BeginTable("models", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Contents");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(view.mModels.size()); ++i)
            {
                const auto &model = view.mModels[i];
                if (!Matches(model, mFilter))
                    continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_OpenOnDoubleClick;
                if (model.mPath == mSelectedPath)
                    flags |= ImGuiTreeNodeFlags_Selected;
                const bool open = ImGui::TreeNodeEx("##model", flags, "%s", model.mPath.c_str());
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                    mSelectedPath = model.mPath;
                if (!scrollTo.empty() && model.mPath == scrollTo)
                    ImGui::SetScrollHereY(0.5f);
                ImGui::TableSetColumnIndex(1);
                DrawStatus(model);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", model.mContents.size());
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginDisabled(view.mProcessRunning);
                if (ImGui::SmallButton(model.mStatus == AssetImportStatus::Failed ? "Retry" : "Reimport"))
                {
                    action.mKind = model.mStatus == AssetImportStatus::Failed ? AssetImportActionKind::Retry
                                                                             : AssetImportActionKind::Reimport;
                    action.mModelPath = model.mPath;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Force"))
                {
                    action.mKind = AssetImportActionKind::ForceReimport;
                    action.mModelPath = model.mPath;
                }
                if (model.mStatus == AssetImportStatus::Missing)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                    {
                        action.mKind = AssetImportActionKind::Remove;
                        action.mModelPath = model.mPath;
                    }
                }
                ImGui::EndDisabled();
                if (open)
                {
                    if (model.mContents.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("    Contents are recorded on import - press Reimport to list them");
                    }
                    int childId = 0;
                    for (const KindRow &kind : kKinds)
                    {
                        bool heading = false;
                        for (const auto &entry : model.mContents)
                        {
                            if (entry.mKind != kind.mKind)
                                continue;
                            if (!heading)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("  %s", kind.mHeading);
                                heading = true;
                            }
                            ImGui::PushID(childId++);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("      %s", entry.mName.c_str());
                            ImGui::TableSetColumnIndex(1);
                            if (entry.mAssetId != 0)
                                ImGui::TextDisabled("%s", HexId(entry.mAssetId).c_str());
                            else
                                ImGui::TextDisabled("index %u", entry.mIndex);
                            ImGui::TableSetColumnIndex(3);
                            if (ImGui::SmallButton(entry.mAssetId != 0 ? "Copy id" : "Copy index"))
                                ImGui::SetClipboardText(entry.mAssetId != 0
                                                            ? HexId(entry.mAssetId).c_str()
                                                            : std::to_string(entry.mIndex).c_str());
                            ImGui::SetItemTooltip("%s", entry.mAssetId != 0
                                                            ? "Asset id for scene JSON or scripts"
                                                            : "Use in ModelPlayer's scene field");
                            ImGui::PopID();
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        const AssetImportModelView *selected = nullptr;
        for (const auto &model : view.mModels)
            if (model.mPath == mSelectedPath)
                selected = &model;
        if (selected != nullptr)
        {
            ImGui::SeparatorText("Selected model");
            ImGui::TextWrapped("%s", selected->mPath.c_str());
            if (selected->mAssetId != 0 && ImGui::SmallButton("Copy model id"))
                ImGui::SetClipboardText(HexId(selected->mAssetId).c_str());
            for (const auto &dependency : selected->mDependencies)
                ImGui::BulletText("%s", dependency.c_str());
            ImGui::InputTextWithHint("Move to", "models/<model-name>", mMoveTarget, sizeof(mMoveTarget));
            ImGui::BeginDisabled(view.mProcessRunning || mMoveTarget[0] == '\0' ||
                                 selected->mStatus == AssetImportStatus::Missing);
            if (ImGui::Button("Move model package"))
            {
                action.mKind = AssetImportActionKind::Move;
                action.mModelPath = selected->mPath;
                action.mTargetDirectory = mMoveTarget;
            }
            ImGui::EndDisabled();
            if (!selected->mDiagnostic.empty())
                ImGui::TextWrapped("%s", selected->mDiagnostic.c_str());
        }

        if (ImGui::TreeNode("Advanced: import by path"))
        {
            ImGui::InputText("glTF / GLB", mSource, sizeof(mSource));
            ImGui::InputText("Package root", mPackageRoot, sizeof(mPackageRoot));
            ImGui::InputTextWithHint("Target", "models/<model-name>", mTargetDirectory, sizeof(mTargetDirectory));
            ImGui::BeginDisabled(view.mProcessRunning || mSource[0] == '\0');
            if (ImGui::Button("Import"))
            {
                action.mKind = AssetImportActionKind::Import;
                action.mExternalSource = mSource;
                action.mPackageRoot = mPackageRoot;
                action.mTargetDirectory = mTargetDirectory;
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        return action;
    }
} // namespace mir
