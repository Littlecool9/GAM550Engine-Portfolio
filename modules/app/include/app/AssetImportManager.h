#pragma once

#include <app/GameProject.h>
#include <core/platform/Process.h>
#include <editor/panels/AssetImportPanel.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace mir
{
    // Editor-side owner for finite AssetCooker batches. It never keeps a
    // cooker process alive while the queue is empty.
    class AssetImportManager
    {
    public:
        bool Initialize(const GameProject &project);
        void Shutdown();
        void Handle(const AssetImportAction &action);
        void Update(bool editing);

        const AssetImportView &View() const { return mView; }
        bool PendingApply() const { return mPendingApply; }
        void MarkApplied();
        void MarkApplyFailed(std::string diagnostic);
        void Reject(const std::string &model, std::string diagnostic);

    private:
        struct Batch
        {
            std::set<std::string> models;
            std::set<std::filesystem::path> changed;
            std::set<std::string> removals;
            std::map<std::string, std::filesystem::path> moves;
            bool force = false;
            std::filesystem::path importSource, packageRoot, targetDirectory;
            bool Empty() const
            {
                return models.empty() && changed.empty() && removals.empty() && moves.empty() && importSource.empty();
            }
            void Clear() { *this = {}; }
        };
        struct WatchedFile
        {
            uint64_t signature = 0;
            std::chrono::steady_clock::time_point changedAt{};
            bool pending = false;
        };

        void StartBatch();
        void FinishBatch(int exitCode, bool editing);
        void QueueExternalImport(const AssetImportAction &action);
        std::filesystem::path UniqueImportTarget(const std::filesystem::path &source) const;
        void RefreshView();
        void PollWatcher();
        void SetStatus(const std::string &model, AssetImportStatus status, std::string diagnostic = {});
        void SaveAutoImport();

        GameProject mProject;
        std::filesystem::path mSourceRoot, mSourceAssetRoot, mSourceSettingsPath, mCooker, mCookedRoot, mResultPath;
        ChildProcess mProcess;
        Batch mQueued, mRunning;
        AssetImportView mView;
        std::unordered_map<std::string, AssetImportStatus> mStatus;
        std::unordered_map<std::string, std::string> mDiagnostics;
        std::map<std::filesystem::path, WatchedFile> mWatched;
        std::chrono::steady_clock::time_point mLastWatchPoll{};
        bool mPendingApply = false;
        bool mCancelRequested = false;
        bool mWatchInitialized = false;
        bool mRunningStale = false;
    };
} // namespace mir
