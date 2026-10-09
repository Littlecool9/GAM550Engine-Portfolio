/**
 * @file PlaySnapshot.cpp
 * @author Sumin Park
 * @brief  Serialization of the world
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/PlaySnapshot.h"

#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "core/platform/Process.h"
#include "scene/SceneIO.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace mir
{
    PlaySnapshot CaptureWorld(World &world, const LoadedScene &scene)
    {
        PlaySnapshot snapshot{ std::filesystem::temp_directory_path() /
                                   ("mjolnir_play_snapshot_" + std::to_string(CurrentProcessId())),
            scene };

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);

        LoadedScene everything = NewScene(scene.mName);
        everything.mNextId = scene.mNextId;
        std::unordered_set<uint64_t> inScene;
        for (const auto &[id, entity] : scene.mEntities)
        {
            if (!world.IsAlive(entity))
                continue;
            everything.mEntities[id] = entity;
            inScene.insert(PackEntity(entity));
        }
        world.ForEachAliveEntity([&](Entity entity)
            {
                if (!inScene.contains(PackEntity(entity)))
                    everything.mEntities[AllocateStableId(everything)] = entity;
            });

        if (!SaveScene(world, snapshot.mDir, everything))
            MIR_LOG_ERROR("CaptureWorld: failed to write Play snapshot to '{}'", snapshot.mDir.string());

        return snapshot;
    }

    LoadedScene RestoreWorld(World &world, const PlaySnapshot &snapshot)
    {
        std::vector<Entity> alive;
        world.ForEachAliveEntity([&](Entity entity)
            {
                alive.push_back(entity);
            });

        for (Entity entity : alive)
        {
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }

        const LoadedScene loaded = LoadScene(world, snapshot.mDir);

        LoadedScene restored = snapshot.mScene;
        restored.mEntities.clear();
        for (const auto &[id, entity] : snapshot.mScene.mEntities)
        {
            if (const auto it = loaded.mEntities.find(id); it != loaded.mEntities.end())
                restored.mEntities[id] = it->second;
        }
        return restored;
    }

    void DiscardSnapshot(PlaySnapshot &snapshot)
    {
        if (snapshot.mDir.empty())
            return;

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);
        snapshot.mDir.clear();
        snapshot.mScene = {};
    }
}
