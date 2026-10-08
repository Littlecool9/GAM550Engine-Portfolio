/**
 * @file SceneResolve.h
 * @author Jackey Cen
 * @brief Post-load pass: saved script names to live script instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <string_view>

namespace mir
{
    class AssetCache;
    class IScriptHost;
    class ScriptReloadWatcher;
    class World;

    // Private to app: App::LoadScene runs this after unloading and releasing.

    // For each ScriptRef without an instance: loads <assetIdRoot>/scripts/<name>.lua and creates one
    void ResolveSceneScripts(World &world, IScriptHost &scripts, ScriptReloadWatcher &reload,
                             AssetCache *assets, std::string_view assetIdRoot);
}
