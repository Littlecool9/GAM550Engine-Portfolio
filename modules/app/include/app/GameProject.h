/**
 * @file GameProject.h
 * @author Jackey Cen
 * @brief A game directory under games/ resolved into what the engine opens it
 *        from: its name, title, asset id root, scene and settings files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    /// Everything the engine derives from one game directory. The paths inside
    /// game.json are relative to that directory, so a game can be renamed or
    /// copied without editing it.
    struct GameProject
    {
        // Executable-relative and '/'-separated, e.g. "games/HelloWorld" - what
        // identifies the game, and what App::OpenProject takes
        std::string mDir;

        // game.json's "name", or the directory name when it has none
        std::string mName;

        std::string mTitle = "MjolnirEngine";

        // "<mDir>/<assetsRoot>" exactly as the cooker hashed it into this
        // game's asset ids, e.g. "games/HelloWorld/assets" - not a disk path
        std::string mAssetIdRoot;

        std::filesystem::path mSceneDir;
        std::filesystem::path mProjectSettingsPath;
    };

    /// engine.json's defaultGame, read from next to the executable - the game
    /// to load when the command line names none. Nullopt (logged) when the
    /// file or the key is missing.
    std::optional<std::string> ReadDefaultGame();

    /// Brings a requested game directory into the one form asset ids can be
    /// built from. The cooker hashed "games/<name>/assets/...", so
    /// "games\<name>\", "./games/<name>" and an absolute path into the run
    /// directory all have to come out as "games/<name>". Nullopt (logged) for
    /// a directory outside the run directory or not there at all.
    std::optional<std::filesystem::path> NormalizeGameDir(const std::string &requested);

    /// Nullopt when `gameDir` has no game.json: that file is what makes a
    /// directory a game (CMakeLists.txt discovers games the same way), and a
    /// wrong directory should stop here rather than open an empty scene. A
    /// game.json that fails to parse still loads, on the defaults, with a
    /// logged reason - a content problem, not a reason not to start.
    std::optional<GameProject> LoadGameProject(const std::filesystem::path &gameDir);

    /// NormalizeGameDir, then LoadGameProject: "open this directory as a game".
    /// Every failure on the way is logged where it happens.
    std::optional<GameProject> ResolveGameProject(const std::string &requested);

    /// Every games/<name>/ next to the executable that has a game.json, sorted
    /// by directory.
    std::vector<GameProject> DiscoverGameProjects();
}
