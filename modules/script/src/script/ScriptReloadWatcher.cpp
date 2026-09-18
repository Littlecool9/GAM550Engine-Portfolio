/**
 * @file ScriptReloadWatcher.cpp
 * @author Rahul Nair
 * @brief Polls a tracked script's cooked file for on-disk changes and
 *        hot-reloads it through IScriptHost.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <script/ScriptReloadWatcher.h>

#include <script/IScriptHost.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <core/log/Log.h>

#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr float kPollIntervalSeconds = 1.0f;
    }

    void ScriptReloadWatcher::Track(std::string scriptName, AssetId id)
    {
        mTracked[std::move(scriptName)] = TrackedScript{id};
    }
    void ScriptReloadWatcher::Poll(AssetCache *cache, float dt)
    {
        if (cache == nullptr || mTracked.empty())
            return;

        mAccumulated += dt;
        if (mAccumulated < kPollIntervalSeconds)
            return;
        mAccumulated = 0.0f;

        for (auto &[name, tracked] : mTracked)
        {
            const std::optional<std::filesystem::path> path = cache->ResolvedPath(tracked.id);
            if (!path.has_value())
                continue;

            std::error_code ec;
            const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(*path, ec);
            if (ec)
                continue;

            if (tracked.hasWriteTime && writeTime == tracked.lastWriteTime)
                continue;

            const bool firstObservation = !tracked.hasWriteTime;
            tracked.lastWriteTime = writeTime;
            tracked.hasWriteTime = true;
            if (firstObservation)
                continue; // establish the baseline silently; nothing has actually changed yet

            cache->Invalidate(tracked.id);
            const AssetBlobView *blob = cache->Load(tracked.id);
            if (blob == nullptr)
            {
                MIR_LOG_ERROR("script: hot-reload: failed to re-load '{}' after it changed on disk", name);
                continue;
            }

            if (mHost.ReloadScriptSource(name, AsStringView(*blob)))
                MIR_LOG_INFO("script: hot-reloaded '{}'", name);
        }
    }
}
