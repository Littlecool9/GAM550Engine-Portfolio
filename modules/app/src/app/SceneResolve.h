/**
 * @file SceneResolve.h
 * @author Jackey Cen
 * @brief Post-load passes that turn the names a scene file saves back into
 *        this run's script instances and GPU handles. Private to app: App
 *        owns the order they run in (unload, release, load, resolve).
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
    class VulkanRenderer;
    class World;

    /// Runs after a scene loads from disk: every ScriptRef LoadScene added has
    /// scriptName set (that's the field SceneIO round-trips) but instanceRef
    /// still at -1 (its default, deliberately not saved - see ScriptRef.h),
    /// because "spawn a live Lua instance" is a side-effecting call SceneIO's
    /// generic field-copy loop has no hook for. This is that hook: for every
    /// such entity, load <assetIdRoot>/scripts/<name>.lua by convention, hand
    /// it to the reload watcher, and wire up the instance ScriptSystem will
    /// then drive. `assets` may be null (no manifest); each miss is logged.
    void ResolveSceneScripts(World &world, IScriptHost &scripts, ScriptReloadWatcher &reload,
                             AssetCache *assets, std::string_view assetIdRoot);

    /// Same shape as ResolveSceneScripts: meshName/materialShader are what
    /// SceneIO round-trips (see MeshRenderer.h), mesh/material are this run's
    /// live GPU handles, still null until this pass fills them in. Handles are
    /// cached by asset id / shader name within one call, so entities sharing a
    /// mesh or material asset share one upload instead of duplicating it.
    void ResolveSceneMeshes(World &world, VulkanRenderer &renderer, AssetCache *assets,
                            std::string_view assetIdRoot);
}
