/**
 * @file SceneResolve.h
 * @author Jackey Cen
 * @brief Post-load passes: saved names to live script instances and GPU handles.
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

    // Private to app: App::LoadScene runs these after unloading and releasing.

    // For each ScriptRef without an instance: loads <assetIdRoot>/scripts/<name>.lua and creates one
    void ResolveSceneScripts(World &world, IScriptHost &scripts, ScriptReloadWatcher &reload,
                             AssetCache *assets, std::string_view assetIdRoot);

    // For each MeshRenderer: uploads its meshName and creates its material; shared assets upload once
    void ResolveSceneMeshes(World &world, VulkanRenderer &renderer, AssetCache *assets,
                            std::string_view assetIdRoot);
}
