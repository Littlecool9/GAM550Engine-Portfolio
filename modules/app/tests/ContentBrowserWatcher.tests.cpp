/**
 * @file ContentBrowserWatcher.tests.cpp
 * @author Rahul Nair
 * @brief Content browser watcher: disk-sync (#152) and mutation coverage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/ContentBrowserWatcher.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <thread>

using namespace mir;

namespace
{
    struct TempProject
    {
        std::filesystem::path root;

        TempProject()
        {
            root = std::filesystem::temp_directory_path() /
                ("content-browser-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "assets");
        }

        ~TempProject() { std::filesystem::remove_all(root); }

        GameProject Project() const
        {
            GameProject project;
            project.mDir = "games/Test";
            project.mName = "Test";
            project.mAssetIdRoot = "games/Test/assets";
            project.mRootDir = root;
            return project;
        }

        void Write(const std::string &relative, const std::string &content = "x") const
        {
            const std::filesystem::path path = root / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path) << content;
        }
    };

    bool HasEntry(const ContentBrowserView &view, const std::string &relativePath)
    {
        for (const auto &entry : view.mEntries)
            if (entry.mRelativePath == relativePath)
                return true;
        return false;
    }

    // longer than the watcher's 250ms poll throttle
    void WaitForNextPoll() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
} // namespace

TEST_CASE("ContentBrowserWatcher lists the content root on Initialize", "[app][content-browser]")
{
    TempProject temp;
    temp.Write("assets/model.glb");
    temp.Write("project_settings.json");

    ContentBrowserWatcher watcher;
    REQUIRE(watcher.Initialize(temp.Project()));
    CHECK(watcher.View().mAvailable);
    CHECK(HasEntry(watcher.View(), "assets/model.glb"));
    CHECK(HasEntry(watcher.View(), "project_settings.json"));
    CHECK(HasEntry(watcher.View(), "assets"));
}

TEST_CASE("ContentBrowserWatcher reflects an external add, delete and rename without reinitializing",
    "[app][content-browser]")
{
    TempProject temp;
    temp.Write("assets/keep.txt");

    ContentBrowserWatcher watcher;
    REQUIRE(watcher.Initialize(temp.Project()));

    temp.Write("assets/added.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK(HasEntry(watcher.View(), "assets/added.txt"));

    std::filesystem::remove(temp.root / "assets" / "added.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK_FALSE(HasEntry(watcher.View(), "assets/added.txt"));

    // A rename is a same-tick remove+add with a matching (size, mtime) signature.
    std::filesystem::rename(temp.root / "assets" / "keep.txt", temp.root / "assets" / "renamed.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK(HasEntry(watcher.View(), "assets/renamed.txt"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/keep.txt"));
    CHECK(watcher.View().mLastRenamedFrom == "assets/keep.txt");
    CHECK(watcher.View().mLastRenamedTo == "assets/renamed.txt");
}

TEST_CASE("ContentBrowserWatcher creates, renames and deletes through Handle", "[app][content-browser]")
{
    TempProject temp;

    ContentBrowserWatcher watcher;
    REQUIRE(watcher.Initialize(temp.Project()));

    ContentBrowserAction create;
    create.mKind = ContentBrowserActionKind::CreateFolder;
    create.mTargetPath = "";
    create.mNewName = "levels";
    watcher.Handle(create);
    CHECK(std::filesystem::is_directory(temp.root / "levels"));
    CHECK(HasEntry(watcher.View(), "levels"));

    ContentBrowserAction rename;
    rename.mKind = ContentBrowserActionKind::Rename;
    rename.mTargetPath = "levels";
    rename.mNewName = "maps";
    watcher.Handle(rename);
    CHECK(std::filesystem::is_directory(temp.root / "maps"));
    CHECK_FALSE(std::filesystem::exists(temp.root / "levels"));

    ContentBrowserAction remove;
    remove.mKind = ContentBrowserActionKind::Delete;
    remove.mTargetPath = "maps";
    watcher.Handle(remove);
    CHECK_FALSE(std::filesystem::exists(temp.root / "maps"));
    CHECK(watcher.View().mLastError.empty());
}

TEST_CASE("ContentBrowserWatcher resolves an asset-relative logical path only under the asset root",
    "[app][content-browser]")
{
    TempProject temp;

    ContentBrowserWatcher watcher;
    REQUIRE(watcher.Initialize(temp.Project()));

    const std::optional<std::string> underAssets = watcher.AssetLogicalPath("assets/models/duck.glb");
    REQUIRE(underAssets.has_value());
    CHECK(*underAssets == "games/Test/assets/models/duck.glb");

    CHECK_FALSE(watcher.AssetLogicalPath("project_settings.json").has_value());
    CHECK_FALSE(watcher.AssetLogicalPath("scenes/default/scene.json").has_value());
}
