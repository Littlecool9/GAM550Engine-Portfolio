/**
 * @file EntitySnapshot.cpp
 * @author Sumin Park
 * @brief Entity capture(+ children) as data.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/EntitySnapshot.h"

#include "EntityCodec.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        constexpr StableId kRootLocalId = 1;
    }

    EntitySnapshot CaptureEntity(World &world, Entity root)
    {
        if (!world.IsAlive(root))
            return {};

        // parents precede children, so replaying SetParent in this order keeps sibling order
        std::vector<Entity> members{ root };
        ForEachDescendant(world, root, [&](Entity entity) { members.push_back(entity); });

        EntityIdMap ids;
        ids.reserve(members.size());
        for (std::size_t i = 0; i < members.size(); ++i)
            ids[PackEntity(members[i])] = kRootLocalId + i;

        // undo and duplicate restore in the same run, so a reference leaving
        // the snapshot keeps its live handle
        json entities = json::array();
        for (std::size_t i = 0; i < members.size(); ++i)
            entities.push_back(EntityToJson(world, members[i], kRootLocalId + i, ids, true));

        return EntitySnapshot{ entities.dump() };
    }

    Entity RestoreEntity(World &world, const EntitySnapshot &snapshot, LoadedScene &scene, Entity parent)
    {
        if (snapshot.mData.empty())
            return kNullEntity;

        std::optional<json> parsed = ParseJson(snapshot.mData, "EntitySnapshot");
        if (!parsed || !parsed->is_array())
        {
            MIR_LOG_ERROR("RestoreEntity: snapshot is not an entity array, nothing restored");
            return kNullEntity;
        }

        std::vector<std::pair<StableId, json>> entities;
        entities.reserve(parsed->size());
        for (json &entity : *parsed)
        {
            if (!entity.is_object())
                continue;

            const StableId id = entity.value("id", kNullStableId);
            entities.emplace_back(id, std::move(entity));
        }

        const std::map<StableId, Entity> spawned = SpawnEntities(world, entities, true);

        for (const auto &[localId, entity] : spawned)
            scene.mEntities[AllocateStableId(scene)] = entity;

        const auto root = spawned.find(kRootLocalId);
        if (root == spawned.end())
            return kNullEntity;

        if (!parent.IsNull())
            SetParent(world, root->second, parent, kNullEntity, false);
        return root->second;
    }

    Entity DuplicateSceneEntity(World &world, LoadedScene &scene, Entity source)
    {
        if (!world.IsAlive(source))
            return kNullEntity;

        // read before the restore, which appends the copy to the same list
        Entity next = kNullEntity;
        if (const HierarchyIndex *index = world.TryResource<HierarchyIndex>())
        {
            const std::span<const Entity> siblings = index->SiblingsOf(source);
            const auto it = std::find(siblings.begin(), siblings.end(), source);
            if (it != siblings.end() && std::next(it) != siblings.end())
                next = *std::next(it);
        }

        const Entity parent = ParentOf(world, source);
        const Entity copy = RestoreEntity(world, CaptureEntity(world, source), scene, parent);

        // right after the source, as Unity's Ctrl+D
        if (!copy.IsNull() && !next.IsNull())
            SetParent(world, copy, parent, next, false);
        return copy;
    }
}
