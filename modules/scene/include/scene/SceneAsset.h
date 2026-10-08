/**
 * @file SceneAsset.h
 * @author Sumin Park
 * @brief The save-file identity for a scene and its entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_set>

namespace mir
{

    // Save-file-only identity for an entity, distinct from Entity struct.
    // this is because Entity's slot and generation get reused across runs
    using StableId = uint64_t;

    // The "no entity" value for a StableId field
    inline constexpr StableId kNullStableId = 0;

    struct LoadedScene
    {
        std::string mName;

        // map, not unordered_map - the manifest is written in key order, so a
        // save with no change produces no diff
        std::map<StableId, Entity> mEntities;

        // next id handed out, persisted in scene.json so a deleted entity's id
        // is never reissued
        StableId mNextId = 1;

        std::set<StableId> mDeletedIds;
    };

    // Not backed by a file until the first save.
    inline LoadedScene NewScene(std::string name)
    {
        LoadedScene scene;
        scene.mName = std::move(name);
        return scene;
    }

    inline StableId AllocateStableId(LoadedScene &scene) { return scene.mNextId++; }

    // Create, allocate a StableId and register the entity
    inline Entity CreateSceneEntity(World &world, LoadedScene &scene, Entity parent = kNullEntity)
    {
        Entity entity = world.CreateEntity();
        scene.mEntities[AllocateStableId(scene)] = entity;
        if (!parent.IsNull())
            SetParent(world, entity, parent);
        return entity;
    }

    inline bool DestroySceneEntity(World &world, LoadedScene &scene, Entity root)
    {
        if (!world.IsAlive(root))
            return false;

        std::unordered_set<uint64_t> doomed{ PackEntity(root) };
        ForEachDescendant(world, root, [&](Entity entity) { doomed.insert(PackEntity(entity)); });

        std::erase_if(scene.mEntities, [&](const auto &entry)
            {
                if (!doomed.contains(PackEntity(entry.second)))
                    return false;
                scene.mDeletedIds.insert(entry.first);
                return true;
            });

        // the hierarchy's destroy hook takes the descendants with it
        world.DestroyEntity(root);
        return true;
    }
}
