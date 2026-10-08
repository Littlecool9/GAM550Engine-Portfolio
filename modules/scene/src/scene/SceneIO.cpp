/**
 * @file SceneIO.cpp
 * @author Sumin Park
 * @brief Save and load a scene as a manifest plus one file per entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneIO.h"

#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "EntityCodec.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        constexpr std::string_view kEntitiesDirName = "entities";

        fs::path EntityFilePath(const fs::path &sceneDir, StableId id)
        {
            return sceneDir / kEntitiesDirName / (std::to_string(id) + ".json");
        }

        void RemoveDeletedEntityFiles(const fs::path &sceneDir, const LoadedScene &scene)
        {
            for (StableId id : scene.mDeletedIds)
            {
                const fs::path file = EntityFilePath(sceneDir, id);
                std::error_code ec;
                fs::remove(file, ec);
                if (ec)
                    MIR_LOG_WARN("SaveScene: could not remove deleted '{}': {}", file.string(), ec.message());
            }
        }
    }

    fs::path SceneManifestPath(const fs::path &sceneDir)
    {
        return sceneDir / kSceneManifestFileName;
    }

    bool SaveScene(World &world, const fs::path &sceneDir, const LoadedScene &scene)
    {
        std::error_code ec;
        fs::create_directories(sceneDir / kEntitiesDirName, ec);
        if (ec)
        {
            MIR_LOG_ERROR("SaveScene: could not create '{}': {}", sceneDir.string(), ec.message());
            return false;
        }

        EntityIdMap ids;
        ids.reserve(scene.mEntities.size());
        for (const auto &[id, entity] : scene.mEntities)
            ids[PackEntity(entity)] = id;

        for (const auto &[id, entity] : scene.mEntities)
        {
            // a file outlives this run, so a reference outside the scene is written null
            if (!WriteJsonFile(EntityFilePath(sceneDir, id), EntityToJson(world, entity, id, ids, false)))
            {
                MIR_LOG_ERROR("SaveScene: could not write entity file for id {}", id);
                return false;
            }
        }

        // The manifest is written last
        json manifest;
        manifest["name"] = scene.mName;
        manifest["nextId"] = scene.mNextId;
        manifest["entities"] = json::array();
        for (const auto &[id, entity] : scene.mEntities)
            manifest["entities"].push_back(id);

        if (!WriteJsonFile(SceneManifestPath(sceneDir), manifest))
        {
            MIR_LOG_ERROR("SaveScene: could not write {} in '{}'", kSceneManifestFileName, sceneDir.string());
            return false;
        }

        RemoveDeletedEntityFiles(sceneDir, scene);
        return true;
    }

    LoadedScene LoadScene(World &world, const fs::path &sceneDir)
    {
        LoadedScene loaded;

        const std::optional<json> manifestJson = ReadJsonFile(SceneManifestPath(sceneDir));
        if (!manifestJson || !manifestJson->is_object())
        {
            MIR_LOG_ERROR("LoadScene: '{}' has no usable {}, loading an empty scene", sceneDir.string(),
                kSceneManifestFileName);
            return loaded;
        }
        const json &manifest = *manifestJson;
        loaded.mName = manifest.value("name", "");
        loaded.mNextId = manifest.value("nextId", StableId{ 1 });

        std::vector<std::pair<StableId, json>> entities;
        for (const auto &idJson : manifest.value("entities", json::array()))
        {
            StableId id = idJson.get<StableId>();

            std::optional<json> entityJson = ReadJsonFile(EntityFilePath(sceneDir, id));
            if (!entityJson || !entityJson->is_object())
            {
                MIR_LOG_ERROR("LoadScene: entity file for id {} listed in manifest but missing or unreadable", id);
                continue; // 0030 leaves this failure mode open; skip rather than abort the load
            }
            entities.emplace_back(id, std::move(*entityJson));
        }

        loaded.mEntities = SpawnEntities(world, entities, false);

        // a hand-edited manifest may carry a nextId that doesn't clear every id
        // present
        if (!loaded.mEntities.empty())
            loaded.mNextId = std::max(loaded.mNextId, loaded.mEntities.rbegin()->first + 1);

        return loaded;
    }

    void UnloadScene(World &world, const LoadedScene &loaded)
    {
        for (const auto &[id, entity] : loaded.mEntities)
        {
            // destroying a parent cascades, so a child reached later may
            // already be dead
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }
    }
}
