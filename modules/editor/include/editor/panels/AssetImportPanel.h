#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    enum class AssetImportStatus
    {
        Ready,
        Queued,
        Processing,
        UpToDate,
        Failed,
        WaitingForStop,
        Applied,
        Missing,
        Canceled,
    };

    struct AssetImportModelView
    {
        std::string mPath;
        uint64_t mAssetId = 0;
        AssetImportStatus mStatus = AssetImportStatus::Ready;
        std::vector<std::string> mDependencies;
        std::string mDiagnostic;
        int64_t mLastSuccess = 0;
    };

    struct AssetImportView
    {
        bool mAvailable = false;
        bool mAutoImport = false;
        bool mProcessRunning = false;
        std::string mUnavailableReason;
        std::vector<AssetImportModelView> mModels;
    };

    enum class AssetImportActionKind
    {
        None,
        BindWorkspace,
        Import,
        Reimport,
        ForceReimport,
        Retry,
        Move,
        Remove,
        Cancel,
        SetAutoImport
    };
    struct AssetImportAction
    {
        AssetImportActionKind mKind = AssetImportActionKind::None;
        std::string mModelPath;
        std::string mExternalSource;
        std::string mPackageRoot;
        std::string mTargetDirectory;
        std::string mWorkspaceRoot;
        bool mEnabled = false;
    };

    class AssetImportPanel
    {
    public:
        AssetImportAction Draw(const AssetImportView &view);

    private:
        char mSource[512]{};
        char mPackageRoot[512]{};
        char mTargetDirectory[256]{};
        char mWorkspaceRoot[512]{};
        char mMoveTarget[256]{};
        int mSelected = -1;
    };
} // namespace mir
