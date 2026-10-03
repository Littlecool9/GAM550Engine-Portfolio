/**
 * @file AssetImportManager.cpp
 * @author Jackey Cen
 * @brief Import batching, source watcher and deferred apply of AssetCooker output.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/AssetImportManager.h>

#include <assets/AssetPath.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;
        bool IsModel(const std::filesystem::path &path)
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".gltf" || ext == ".glb";
        }
        bool IsWatched(const std::filesystem::path &path)
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".gltf" || ext == ".glb" || ext == ".bin" || ext == ".png" || ext == ".jpg" || ext == ".jpeg";
        }
        // The key AssetCooker uses for a model under the source asset root
        AssetPathResult LogicalModelPath(std::string_view idRoot, const std::filesystem::path &relative)
        {
            return MakeLogicalAssetPath(idRoot, relative.generic_string());
        }
        uint64_t Signature(const std::filesystem::directory_entry &entry)
        {
            std::error_code error;
            const auto time = entry.last_write_time(error).time_since_epoch().count();
            const auto size = entry.file_size(error);
            return static_cast<uint64_t>(time) ^ (size + 0x9e3779b97f4a7c15ull + (static_cast<uint64_t>(time) << 6));
        }
        Json ReadJson(const std::filesystem::path &path, Json fallback = Json::object())
        {
            if (!std::filesystem::exists(path))
                return fallback;
            std::ifstream input(path);
            Json value;
            input >> value;
            return value;
        }
        void WriteJson(const std::filesystem::path &path, const Json &value)
        {
            const auto pending = path.string() + ".pending";
            std::ofstream(pending, std::ios::trunc) << value.dump(2) << '\n';
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::rename(pending, path);
        }
    } // namespace

    bool AssetImportManager::Initialize(const GameProject &project)
    {
        mProject = project;
        mView = {};
        try
        {
            const Json binding = ReadJson(ExecutableDir() / "asset_workspace.json");
            if (binding.value("version", 0) != 1)
                throw std::runtime_error("asset_workspace.json is missing or has an unsupported version");
            mSourceRoot = binding.at("sourceRoot").get<std::string>();
            mCooker = binding.at("assetCooker").get<std::string>();
            mCookedRoot = CookedAssetsDir();
            mSourceAssetRoot = mSourceRoot / std::filesystem::path(project.mAssetIdRoot);
            mSourceSettingsPath = mSourceRoot / std::filesystem::path(project.mDir) / "project_settings.json";
            mResultPath = mCookedRoot / ".asset-import-result.json";
            if (!std::filesystem::is_directory(mSourceAssetRoot))
                throw std::runtime_error("source asset root does not exist: " + mSourceAssetRoot.string());
            if (!std::filesystem::is_regular_file(mCooker))
                throw std::runtime_error("AssetCooker executable does not exist: " + mCooker.string());
            mView.mAvailable = true;
            mView.mWatchedRoot = mSourceAssetRoot.generic_string();
            const Json settings = ReadJson(mSourceSettingsPath);
            mView.mAutoImport = settings.value("assets", Json::object()).value("autoImport", false);
            RefreshView();
            PollWatcher();
            return true;
        }
        catch (const std::exception &error)
        {
            mView.mAvailable = false;
            mView.mUnavailableReason = error.what();
            MIR_LOG_WARN("Asset importing unavailable: {}", error.what());
            return false;
        }
    }

    void AssetImportManager::Shutdown()
    {
        if (mProcess.Running())
            mProcess.Terminate();
        mView.mProcessRunning = false;
    }

    void AssetImportManager::SetStatus(const std::string &model, AssetImportStatus status, std::string diagnostic)
    {
        mStatus[model] = status;
        if (!diagnostic.empty())
            mDiagnostics[model] = std::move(diagnostic);
    }

    void AssetImportManager::Handle(const AssetImportAction &action)
    {
        if (action.mKind == AssetImportActionKind::BindWorkspace)
        {
            try
            {
                const std::filesystem::path source = std::filesystem::weakly_canonical(action.mWorkspaceRoot);
                if (!std::filesystem::is_directory(source / "games"))
                    throw std::runtime_error("the selected directory has no games directory");
                const auto cooker = ExecutableDir() / "tools" / "AssetCooker" / "AssetCooker.exe";
                if (!std::filesystem::is_regular_file(cooker))
                    throw std::runtime_error("AssetCooker is not available in this build");
                WriteJson(ExecutableDir() / "asset_workspace.json", {{"version", 1},
                                                                     {"sourceRoot", source.generic_string()},
                                                                     {"assetCooker", cooker.generic_string()}});
                Initialize(mProject);
            }
            catch (const std::exception &error)
            {
                mView.mUnavailableReason = error.what();
            }
            return;
        }
        if (!mView.mAvailable || action.mKind == AssetImportActionKind::None)
            return;
        mView.mLastError.clear();
        switch (action.mKind)
        {
        case AssetImportActionKind::BindWorkspace:
            break;
        case AssetImportActionKind::Import:
            QueueExternalImport(action);
            break;
        case AssetImportActionKind::Reimport:
        case AssetImportActionKind::Retry:
            mQueued.models.insert(action.mModelPath);
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::ForceReimport:
            mQueued.models.insert(action.mModelPath);
            mQueued.force = true;
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Remove:
            mQueued.removals.insert(action.mModelPath);
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Move:
            mQueued.moves[action.mModelPath] = action.mTargetDirectory;
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Cancel:
            if (mProcess.Running())
            {
                mCancelRequested = true;
                mProcess.Terminate();
            }
            else
            {
                for (const auto &model : mQueued.models)
                    SetStatus(model, AssetImportStatus::Canceled);
                for (const auto &model : mQueued.removals)
                    SetStatus(model, AssetImportStatus::Canceled);
                for (const auto &[model, destination] : mQueued.moves)
                    SetStatus(model, AssetImportStatus::Canceled);
                mQueued.Clear();
            }
            break;
        case AssetImportActionKind::SetAutoImport:
            mView.mAutoImport = action.mEnabled;
            SaveAutoImport();
            break;
        case AssetImportActionKind::None:
            break;
        }
        RefreshView();
    }

    std::filesystem::path AssetImportManager::UniqueImportTarget(const std::filesystem::path &source) const
    {
        const std::string stem = source.stem().string();
        std::filesystem::path target = std::filesystem::path("models") / stem;
        for (int suffix = 2; std::filesystem::exists(mSourceAssetRoot / target); ++suffix)
            target = std::filesystem::path("models") / (stem + "-" + std::to_string(suffix));
        return target;
    }

    void AssetImportManager::QueueExternalImport(const AssetImportAction &action)
    {
        try
        {
            auto source = std::filesystem::weakly_canonical(action.mExternalSource);
            std::filesystem::path package;
            if (!action.mPackageRoot.empty())
                package = std::filesystem::weakly_canonical(action.mPackageRoot);
            if (std::filesystem::is_directory(source))
            {
                // A dropped folder is the package root
                std::vector<std::filesystem::path> entries;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(source))
                    if (entry.is_regular_file() && IsModel(entry.path()))
                        entries.push_back(entry.path());
                if (entries.empty())
                    throw std::runtime_error("no .gltf or .glb file in " + source.string());
                if (entries.size() > 1)
                    throw std::runtime_error(source.string() + " holds " + std::to_string(entries.size()) +
                                             " glTF files; drop the one you want instead of the folder");
                package = source;
                source = entries.front();
            }
            if (!std::filesystem::is_regular_file(source))
                throw std::runtime_error("no such file: " + source.string());
            if (!IsModel(source))
                throw std::runtime_error("not a .gltf or .glb file: " + source.string());

            // Already inside the project: re-import it in place rather than
            // copying a second instance of the same package.
            const auto relative = source.lexically_relative(std::filesystem::weakly_canonical(mSourceAssetRoot));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
            {
                const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative);
                if (!logical.Ok())
                    throw std::runtime_error(source.string() + ": " + std::string(Describe(logical.error)));
                mQueued.models.insert(logical.path);
                SetStatus(logical.path, AssetImportStatus::Queued);
                return;
            }
            mQueued.importSource = source;
            mQueued.packageRoot = package.empty() ? source.parent_path() : package;
            mQueued.targetDirectory = action.mTargetDirectory.empty()
                                          ? UniqueImportTarget(source)
                                          : std::filesystem::path(action.mTargetDirectory);
        }
        catch (const std::exception &error)
        {
            mView.mLastError = error.what();
            MIR_LOG_ERROR("Asset import rejected: {}", error.what());
        }
    }

    void AssetImportManager::StartBatch()
    {
        if (mQueued.Empty() || mProcess.Running())
            return;
        mRunning = std::move(mQueued);
        mQueued.Clear();
        std::vector<std::string> args = {"--source-dir", mSourceAssetRoot.string(), "--id-root", mProject.mAssetIdRoot,
                                         "--out",        mCookedRoot.string(),      "--result",  mResultPath.string()};
        for (const auto &model : mRunning.models)
        {
            args.push_back("--model");
            args.push_back(model);
            SetStatus(model, AssetImportStatus::Processing);
        }
        for (const auto &changed : mRunning.changed)
        {
            args.push_back("--changed");
            args.push_back(changed.string());
        }
        for (const auto &model : mRunning.removals)
        {
            args.push_back("--remove-model");
            args.push_back(model);
        }
        for (const auto &[model, destination] : mRunning.moves)
            args.insert(args.end(), {"--move-model", model, "--move-to", destination.string()});
        if (mRunning.force)
            args.push_back("--force");
        if (!mRunning.importSource.empty())
        {
            args.insert(args.end(), {"--import", mRunning.importSource.string()});
            if (!mRunning.packageRoot.empty())
                args.insert(args.end(), {"--package-root", mRunning.packageRoot.string()});
            if (!mRunning.targetDirectory.empty())
                args.insert(args.end(), {"--destination", mRunning.targetDirectory.string()});
        }
        std::error_code error;
        std::filesystem::remove(mResultPath, error);
        if (!mProcess.Start(mCooker, args, ExecutableDir()))
        {
            for (const auto &model : mRunning.models)
                SetStatus(model, AssetImportStatus::Failed, "could not start AssetCooker");
            mRunning.Clear();
            return;
        }
        mView.mProcessRunning = true;
        mCancelRequested = false;
        mRunningStale = false;
        MIR_LOG_INFO("Started AssetCooker for {} queued model(s)", mRunning.models.size());
    }

    void AssetImportManager::FinishBatch(int exitCode, bool editing)
    {
        mView.mProcessRunning = false;
        if (mCancelRequested)
        {
            for (const auto &model : mRunning.models)
                SetStatus(model, AssetImportStatus::Canceled);
            for (const auto &model : mRunning.removals)
                SetStatus(model, AssetImportStatus::Canceled);
            for (const auto &[model, destination] : mRunning.moves)
                SetStatus(model, AssetImportStatus::Canceled);
        }
        else
        {
            try
            {
                const Json result = ReadJson(mResultPath);
                if (exitCode != 0 || result.value("status", "failed") == "failed")
                {
                    const std::string message =
                        result.value("diagnostic", Json::object()).value("message", "AssetCooker failed");
                    for (const auto &model : mRunning.models)
                        SetStatus(model, AssetImportStatus::Failed, message);
                    mView.mLastError = message;
                    MIR_LOG_ERROR("Asset import failed: {}", message);
                }
                else
                {
                    bool cooked = false;
                    const Json resultModels = result.value("models", Json::array());
                    for (const auto &model : resultModels)
                    {
                        const std::string path = model.value("path", "");
                        const std::string status = model.value("status", "");
                        if (status == "cooked" || status == "removed")
                            cooked = true;
                        if (status == "cooked" && !mRunningStale)
                            mView.mLastImported = path;
                        SetStatus(path, mRunningStale           ? AssetImportStatus::Queued
                                        : status == "missing"   ? AssetImportStatus::Missing
                                        : status == "unchanged" ? AssetImportStatus::UpToDate
                                                                : (editing ? AssetImportStatus::Applied
                                                                           : AssetImportStatus::WaitingForStop));
                    }
                    mPendingApply |= cooked && !mRunningStale;
                }
            }
            catch (const std::exception &error)
            {
                for (const auto &model : mRunning.models)
                    SetStatus(model, AssetImportStatus::Failed, error.what());
            }
        }
        if (mRunningStale)
        {
            mQueued.models.insert(mRunning.models.begin(), mRunning.models.end());
            mQueued.changed.insert(mRunning.changed.begin(), mRunning.changed.end());
        }
        if (!mRunning.importSource.empty() || !mRunning.moves.empty())
        {
            mWatched.clear();
            mWatchInitialized = false;
            mLastWatchPoll = {};
        }
        mRunning.Clear();
        RefreshView();
    }

    void AssetImportManager::PollWatcher()
    {
        if (!mView.mAvailable)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (mLastWatchPoll != std::chrono::steady_clock::time_point{} &&
            now - mLastWatchPoll < std::chrono::milliseconds(250))
            return;
        mLastWatchPoll = now;
        std::set<std::filesystem::path> present;
        const bool cookerChangesSource =
            mView.mProcessRunning && (!mRunning.importSource.empty() || !mRunning.moves.empty());
        for (const auto &entry : std::filesystem::recursive_directory_iterator(mSourceAssetRoot))
        {
            if (!entry.is_regular_file() || !IsWatched(entry.path()))
                continue;
            const auto path = std::filesystem::weakly_canonical(entry.path());
            present.insert(path);
            const uint64_t signature = Signature(entry);
            auto [it, inserted] = mWatched.try_emplace(path, WatchedFile{signature, now, false});
            if (inserted && mWatchInitialized)
            {
                it->second.changedAt = now;
                it->second.pending = true;
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
            }
            if (!inserted && it->second.signature != signature)
            {
                it->second.signature = signature;
                it->second.changedAt = now;
                it->second.pending = true;
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
            }
            if (mView.mAutoImport && it->second.pending && now - it->second.changedAt >= std::chrono::milliseconds(500))
            {
                if (IsModel(path))
                {
                    const auto relative = std::filesystem::relative(path, mSourceAssetRoot);
                    if (const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative); logical.Ok())
                    {
                        mQueued.models.insert(logical.path);
                        SetStatus(logical.path, AssetImportStatus::Queued);
                    }
                    else
                        MIR_LOG_ERROR("Asset import: '{}': {}", path.string(), Describe(logical.error));
                }
                else
                    mQueued.changed.insert(path);
                it->second.pending = false;
            }
        }
        for (auto it = mWatched.begin(); it != mWatched.end();)
        {
            if (!present.contains(it->first))
            {
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
                if (mView.mAutoImport)
                    mQueued.changed.insert(it->first);
                it = mWatched.erase(it);
            }
            else
                ++it;
        }
        mWatchInitialized = true;
    }

    void AssetImportManager::Update(bool editing)
    {
        if (!mView.mAvailable)
            return;
        PollWatcher();
        if (mView.mProcessRunning)
            if (auto code = mProcess.Poll())
                FinishBatch(*code, editing);
        if (!mView.mProcessRunning)
            StartBatch();
        RefreshView();
    }

    void AssetImportManager::RefreshView()
    {
        if (!mView.mAvailable)
            return;
        std::map<std::string, AssetImportModelView> models;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(mSourceAssetRoot))
            if (entry.is_regular_file() && IsModel(entry.path()))
            {
                // Runs every frame: an unusable name is logged once, when the watcher sees it
                const auto relative = std::filesystem::relative(entry.path(), mSourceAssetRoot);
                if (const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative); logical.Ok())
                    models[logical.path].mPath = logical.path;
            }
        try
        {
            const Json cache = ReadJson(mCookedRoot / "import-cache.json");
            const Json cachedModels = cache.value("models", Json::object());
            for (const auto &[path, record] : cachedModels.items())
            {
                auto &model = models[path];
                model.mPath = path;
                model.mAssetId = record.value("modelId", 0ull);
                model.mLastSuccess = record.value("lastSuccess", 0ll);
                model.mStatus =
                    record.value("missing", false) ? AssetImportStatus::Missing : AssetImportStatus::UpToDate;
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    model.mDependencies.push_back(dependency.get<std::string>());
                for (const auto &entry : record.value("contents", Json::array()))
                    model.mContents.push_back({entry.value("kind", std::string{}), entry.value("name", std::string{}),
                                               entry.value("id", 0ull), entry.value("index", 0u)});
                model.mDiagnostic = record.value("lastError", "");
            }
        }
        catch (const std::exception &error)
        {
            MIR_LOG_WARN("Could not read import cache: {}", error.what());
        }
        for (auto &[path, model] : models)
        {
            if (auto status = mStatus.find(path); status != mStatus.end())
                model.mStatus = status->second;
            if (auto diagnostic = mDiagnostics.find(path); diagnostic != mDiagnostics.end())
                model.mDiagnostic = diagnostic->second;
        }
        mView.mModels.clear();
        for (auto &[path, model] : models)
            mView.mModels.push_back(std::move(model));
    }

    void AssetImportManager::SaveAutoImport()
    {
        try
        {
            Json settings = ReadJson(mSourceSettingsPath);
            settings["assets"]["autoImport"] = mView.mAutoImport;
            WriteJson(mSourceSettingsPath, settings);
        }
        catch (const std::exception &error)
        {
            MIR_LOG_ERROR("Could not save auto import setting: {}", error.what());
        }
    }

    void AssetImportManager::MarkApplied()
    {
        mPendingApply = false;
        for (auto &[path, status] : mStatus)
            if (status == AssetImportStatus::WaitingForStop)
                status = AssetImportStatus::Applied;
        RefreshView();
    }
    void AssetImportManager::MarkApplyFailed(std::string diagnostic)
    {
        mPendingApply = false;
        for (auto &[path, status] : mStatus)
            if (status == AssetImportStatus::Applied || status == AssetImportStatus::WaitingForStop)
                SetStatus(path, AssetImportStatus::Failed, diagnostic);
        RefreshView();
    }

    void AssetImportManager::Reject(const std::string &model, std::string diagnostic)
    {
        SetStatus(model, AssetImportStatus::Failed, std::move(diagnostic));
        RefreshView();
    }
} // namespace mir
