/**
 * @file ScriptReloadWatcher.h
 * @author Rahul Nair
 * @brief Polls a tracked script's cooked file for on-disk changes and
 *        hot-reloads it through IScriptHost.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace mir
{
    class IScriptHost;
    class AssetCache;

    /**
     * Polls the cooked file backing each tracked script for on-disk changes,
     * on a coarse timer - not a filesystem watch, since nothing in the
     * engine has async/watch infra to build one on top of. Reloading a
     * script is "load and run a new chunk", so once a change is seen this is
     * a thin wrapper over AssetCache::Invalidate + IScriptHost::ReloadScriptSource.
     *
     * Poll takes the AssetCache by pointer each call rather than storing a
     * reference, so this object has no lifetime dependency on App's lazily-
     * constructed cache - a null cache (assets unavailable this run) just
     * skips the poll.
     */
    class ScriptReloadWatcher
    {
    public:
        explicit ScriptReloadWatcher(IScriptHost &host) : mHost(host)
        {
        }

        void Track(std::string scriptName, AssetId id);
        void Poll(AssetCache *cache, float dt);

    private:
        struct TrackedScript
        {
            AssetId id;
            std::filesystem::file_time_type lastWriteTime{};
            bool hasWriteTime = false;
        };

        IScriptHost &mHost;
        std::unordered_map<std::string, TrackedScript> mTracked;
        float mAccumulated = 0.0f;
    };
}
