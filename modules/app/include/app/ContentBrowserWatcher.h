/**
 * @file ContentBrowserWatcher.h
 * @author Rahul Nair
 * @brief Editor-side content browser: polls the open project's content root
 *        for disk changes and carries out folder/file create, rename and delete.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <app/GameProject.h>
#include <editor/panels/ContentBrowserPanel.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace mir
{
    class ContentBrowserWatcher
    {
    public:
        bool Initialize(const GameProject &project);
        void Update();
        void Handle(const ContentBrowserAction &action);
        void Reject(const std::string &path, std::string diagnostic);

        const ContentBrowserView &View() const { return mView; }

        std::optional<std::string> AssetLogicalPath(const std::string &relativePath) const;

    private:
        struct WatchedFile
        {
            uint64_t signature = 0;
        };

        void Refresh();
        void PollWatcher();
        void CreateFolder(const std::string &parentRelative, const std::string &name);
        void RenameEntry(const std::string &targetRelative, const std::string &newName);
        void DeleteEntry(const std::string &targetRelative);

        GameProject mProject;
        std::filesystem::path mRoot;
        std::map<std::filesystem::path, WatchedFile> mWatched;
        std::chrono::steady_clock::time_point mLastPoll{};
        bool mWatchInitialized = false;
        ContentBrowserView mView;
    };
} // namespace mir
