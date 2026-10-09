/**
 * @file SceneResolve.cpp
 * @author Jackey Cen
 * @brief Post-load pass: saved script names to live script instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SceneResolve.h"

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <script/IScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <script/components/ScriptRef.h>

#include <string>

namespace mir
{
    namespace
    {
        // Loads a cooked .lua and tracks it for hot reload; false if unavailable (already logged)
        bool LoadScriptAsset(IScriptHost &scripts, ScriptReloadWatcher &reload, AssetCache *assets,
            std::string_view assetPath, std::string_view scriptName)
        {
            if (assets == nullptr)
                return false;

            const AssetId id = MakeAssetId(assetPath);
            const AssetBlobView *blob = assets->Load(id);
            if (blob == nullptr)
                return false;

            if (!scripts.LoadScriptSource(scriptName, AsStringView(*blob)))
                return false;

            reload.Track(std::string(scriptName), id);
            return true;
        }
    }

    void ResolveSceneScripts(World &world, IScriptHost &scripts, ScriptReloadWatcher &reload,
        AssetCache *assets, std::string_view assetIdRoot)
    {
        world.GetOrCreateQuery<ScriptRef>().ForEach(
            [&](Entity, ScriptRef &ref)
            {
                if (ref.instanceRef >= 0 || ref.scriptName[0] == '\0')
                    return;

                const std::string name = ref.scriptName;
                const AssetPathResult assetPath = MakeLogicalAssetPath(assetIdRoot, "scripts/" + name + ".lua");
                if (!assetPath.Ok())
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: script '{}': {}", name, Describe(assetPath.error));
                    return;
                }

                if (!LoadScriptAsset(scripts, reload, assets, assetPath.path, name))
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: could not load '{}' for script '{}'", assetPath.path, name);
                    return;
                }

                ref.instanceRef = scripts.CreateInstance(name);
            });
    }
}
