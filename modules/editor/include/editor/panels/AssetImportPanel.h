#pragma once

#include <cstdint>
#include <span>
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

    struct AssetImportSubAsset
    {
        std::string mKind;
        std::string mName;
        uint64_t mAssetId = 0;
        uint32_t mIndex = 0;
    };

    struct AssetImportModelView
    {
        std::string mPath;
        uint64_t mAssetId = 0;
        AssetImportStatus mStatus = AssetImportStatus::Ready;
        std::vector<std::string> mDependencies;
        std::vector<AssetImportSubAsset> mContents;
        std::string mDiagnostic;
        int64_t mLastSuccess = 0;
    };

    struct AssetImportView
    {
        bool mAvailable = false;
        bool mAutoImport = false;
        bool mProcessRunning = false;
        std::string mUnavailableReason;
        // showed the tree which automatic import react to
        std::string mWatchedRoot;

        std::string mLastError;
        std::string mLastImported;
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
        AssetImportAction Draw(const AssetImportView &view, std::span<const std::string> dropped);

    private:
        // A dropped package waiting for confirmation.
        struct Staged
        {
            std::string mSource;
            std::string mPackageRoot;
            std::string mTarget;
        };

        void AcceptDrops(std::span<const std::string> dropped);

        char mSource[512]{};
        char mPackageRoot[512]{};
        char mTargetDirectory[256]{};
        char mWorkspaceRoot[512]{};
        char mMoveTarget[256]{};
        char mFilter[64]{};
        std::vector<Staged> mStaged;
        std::string mDropError;
        std::string mSelectedPath;
        std::string mScrolledTo;
        bool mImportAll = false;
    };
} // namespace mir
