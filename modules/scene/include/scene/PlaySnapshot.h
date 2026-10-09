/**
 * @file PlaySnapshot.h
 * @author Sumin Park
 * @brief  Serialization of the world
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "scene/SceneAsset.h"

#include <filesystem>

namespace mir
{
    class World;

    struct PlaySnapshot
    {
        std::filesystem::path mDir;
        LoadedScene mScene;
    };

    PlaySnapshot CaptureWorld(World &world, const LoadedScene &scene);

    LoadedScene RestoreWorld(World &world, const PlaySnapshot &snapshot);

    // Removes the snapshot's directory, one per process, once it will not be
    // restored again; the snapshot is empty afterwards
    void DiscardSnapshot(PlaySnapshot &snapshot);
}
