/**
 * @file GameProject.h
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
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
    // One game directory; paths in its game.json are relative to it
    struct GameProject
    {
        // relative to the executable, e.g. "games/HelloWorld"
        std::string mDir;
        
        // game.json's "name", else the directory name
        std::string mName;

        std::string mTitle = "MjolnirEngine";

        // asset id prefix, e.g. "games/HelloWorld/assets" - not a disk path
        std::string mAssetIdRoot;

        std::filesystem::path mSceneDir;
        std::filesystem::path mProjectSettingsPath;
        std::filesystem::path mSoundBankDir;
    };

    // engine.json's defaultGame; nullopt (logged) if missing
    std::optional<std::string> ReadDefaultGame();

    // To the executable-relative form asset ids use ("games/<name>");
    // nullopt (logged) if outside the executable directory or missing
    std::optional<std::filesystem::path> NormalizeGameDir(const std::string &requested);

    // Nullopt (logged) without a game.json; a malformed one falls back to defaults
    std::optional<GameProject> LoadGameProject(const std::filesystem::path &gameDir);

    // NormalizeGameDir, then LoadGameProject
    std::optional<GameProject> ResolveGameProject(const std::string &requested);

    // Every games/*/ next to the executable with a game.json, sorted by directory
    std::vector<GameProject> DiscoverGameProjects();

    // Writes games/<name>/: game.json, an EmptyScene-shaped scenes/default/scene.json
    bool CreateGameProject(const std::string &name, bool useRayTracing);
}
