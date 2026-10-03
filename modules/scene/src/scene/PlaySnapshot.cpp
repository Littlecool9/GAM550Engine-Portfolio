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
#include <vector>

namespace mir
{
    PlaySnapshot CaptureWorld(World &world)
    {

        PlaySnapshot snapshot{ std::filesystem::temp_directory_path() /
                               ("mjolnir_play_snapshot_" + std::to_string(CurrentProcessId())) };

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);

        LoadedScene scene = NewScene("__play_snapshot");
        world.ForEachAliveEntity([&](Entity entity)
            {
                scene.mEntities[AllocateStableId(scene)] = entity;
            });

        if (!SaveScene(world, snapshot.mDir, scene))
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

        return LoadScene(world, snapshot.mDir);
    }

    void DiscardSnapshot(PlaySnapshot &snapshot)
    {
        if (snapshot.mDir.empty())
            return;

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);
        snapshot.mDir.clear();
    }
}
