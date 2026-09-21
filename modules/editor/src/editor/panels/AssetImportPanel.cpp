#include <editor/panels/AssetImportPanel.h>

#include <imgui.h>

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
    } // namespace

    AssetImportAction AssetImportPanel::Draw(const AssetImportView &view)
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

        bool automatic = view.mAutoImport;
        if (ImGui::Checkbox("Automatically import changed assets", &automatic))
        {
            action.mKind = AssetImportActionKind::SetAutoImport;
            action.mEnabled = automatic;
        }
        ImGui::SeparatorText("Import external glTF package");
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
        ImGui::SameLine();
        if (view.mProcessRunning && ImGui::Button("Cancel current batch"))
            action.mKind = AssetImportActionKind::Cancel;

        ImGui::SeparatorText("Managed models");
        if (ImGui::BeginTable("models", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Dependencies");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(view.mModels.size()); ++i)
            {
                const auto &model = view.mModels[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(model.mPath.c_str(), mSelected == i, ImGuiSelectableFlags_SpanAllColumns))
                    mSelected = i;
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(StatusName(model.mStatus));
                if (!model.mDiagnostic.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", model.mDiagnostic.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", model.mDependencies.size());
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
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (mSelected >= 0 && mSelected < static_cast<int>(view.mModels.size()))
        {
            const auto &selected = view.mModels[mSelected];
            ImGui::SeparatorText("Selected model");
            ImGui::TextWrapped("%s", selected.mPath.c_str());
            for (const auto &dependency : selected.mDependencies)
                ImGui::BulletText("%s", dependency.c_str());
            ImGui::InputTextWithHint("Move to", "models/<model-name>", mMoveTarget, sizeof(mMoveTarget));
            ImGui::BeginDisabled(view.mProcessRunning || mMoveTarget[0] == '\0' ||
                                 selected.mStatus == AssetImportStatus::Missing);
            if (ImGui::Button("Move model package"))
            {
                action.mKind = AssetImportActionKind::Move;
                action.mModelPath = selected.mPath;
                action.mTargetDirectory = mMoveTarget;
            }
            ImGui::EndDisabled();
            if (!selected.mDiagnostic.empty())
                ImGui::TextWrapped("%s", selected.mDiagnostic.c_str());
        }
        return action;
    }
} // namespace mir
