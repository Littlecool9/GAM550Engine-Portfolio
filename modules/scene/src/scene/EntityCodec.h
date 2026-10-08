/**
 * @file EntityCodec.h
 * @author Sumin Park
 * @brief Entity <-> Json converter
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    class World;

    using EntityIdMap = std::unordered_map<uint64_t, StableId>;

    nlohmann::json EntityToJson(World &world, Entity entity, StableId id, const EntityIdMap &ids,
        bool keepOutsideRefs);

    std::map<StableId, Entity> SpawnEntities(World &world,
        const std::vector<std::pair<StableId, nlohmann::json>> &entities, bool resolveOutsideRefs);
}
