/**
 * @file EntitySnapshot.h
 * @author Sumin Park
 * @brief Capture an entity and its descendants as data, and rebuild them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <string>

namespace mir
{
    class World;

    struct EntitySnapshot
    {
        std::string mData;
    };

    EntitySnapshot CaptureEntity(World &world, Entity root);

    Entity RestoreEntity(World &world, const EntitySnapshot &snapshot, LoadedScene &scene,
        Entity parent = kNullEntity);

    Entity DuplicateSceneEntity(World &world, LoadedScene &scene, Entity source);
}
