/**
 * @file SceneResolve.cpp
 * @author Jackey Cen
 * @brief Post-load passes that turn the names a scene file saves back into
 *        this run's script instances and GPU handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SceneResolve.h"

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/components/MeshRenderer.h>
#include <script/IScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <script/components/ScriptRef.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        /// Loads a `.lua` asset by its cooked path and registers it with the
        /// script host under `scriptName`, then hands it to the reload watcher
        /// so an edit followed by a rebuild is picked up without restarting.
        /// False when assets aren't available or the file failed to load or
        /// parse - logged by AssetCache/ScriptHost already, so the caller just
        /// decides whether to attach the script.
        bool LoadScriptAsset(IScriptHost &scripts, ScriptReloadWatcher &reload, AssetCache *assets,
                             std::string_view assetPath, std::string_view scriptName)
        {
            if (assets == nullptr)
                return false;

            const AssetId id = MakeAssetId(assetPath);
            const AssetBlobView *blob = assets->Load(id);
            if (blob == nullptr)
                return false;

            if (!scripts.LoadScriptSource(scriptName, AsStringView(*blob)))
                return false;

            reload.Track(std::string(scriptName), id);
            return true;
        }

        /// A mesh asset is just MeshData (Shapes.h) as JSON - vertices (position/
        /// color/normal) and indices, the same shape CreateMesh already takes.
        /// No importer, no binary format: hand- or tool-authored JSON is enough
        /// until there's a reason for more. Malformed JSON logs and returns
        /// nullopt rather than throwing - one bad mesh asset shouldn't take the
        /// whole load down.
        std::optional<MeshData> ParseMeshAsset(std::string_view text, std::string_view assetPath)
        {
            nlohmann::json json;
            try
            {
                json = nlohmann::json::parse(text);
            }
            catch (const nlohmann::json::parse_error &e)
            {
                MIR_LOG_ERROR("ParseMeshAsset: '{}' is not valid JSON ({})", assetPath, e.what());
                return std::nullopt;
            }

            MeshData mesh;
            for (const auto &v : json.value("vertices", nlohmann::json::array()))
            {
                Vertex vertex{};
                const auto &p = v.at("position");
                const auto &c = v.at("color");
                const auto &n = v.at("normal");
                vertex.pos = glm::vec3(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
                vertex.color = glm::vec3(c.at(0).get<float>(), c.at(1).get<float>(), c.at(2).get<float>());
                vertex.normal = glm::vec3(n.at(0).get<float>(), n.at(1).get<float>(), n.at(2).get<float>());
                mesh.vertices.push_back(vertex);
            }
            mesh.indices = json.value("indices", std::vector<uint32_t>{});
            return mesh;
        }
    }

    void ResolveSceneScripts(World &world, IScriptHost &scripts, ScriptReloadWatcher &reload,
                             AssetCache *assets, std::string_view assetIdRoot)
    {
        world.GetOrCreateQuery<ScriptRef>().ForEach(
            [&](Entity, ScriptRef &ref)
            {
                if (ref.instanceRef >= 0 || ref.scriptName[0] == '\0')
                    return;

                const std::string name = ref.scriptName;
                const std::string assetPath = std::string(assetIdRoot) + "/scripts/" + name + ".lua";

                if (!LoadScriptAsset(scripts, reload, assets, assetPath, name))
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: could not load '{}' for script '{}'", assetPath, name);
                    return;
                }

                ref.instanceRef = scripts.CreateInstance(name);
            });
    }

    void ResolveSceneMeshes(World &world, VulkanRenderer &renderer, AssetCache *assets,
                            std::string_view assetIdRoot)
    {
        std::unordered_map<uint64_t, MeshHandle> meshCache;
        std::unordered_map<std::string, MaterialHandle> materialCache;

        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity, MeshRenderer &meshRenderer)
            {
                if (meshRenderer.mesh.IsNull() && meshRenderer.meshName[0] != '\0')
                {
                    const std::string assetPath = std::string(assetIdRoot) + "/" + meshRenderer.meshName;
                    const AssetId id = MakeAssetId(assetPath);

                    if (auto it = meshCache.find(id.value); it != meshCache.end())
                    {
                        meshRenderer.mesh = it->second;
                    }
                    else if (assets != nullptr)
                    {
                        if (const AssetBlobView *blob = assets->Load(id); blob != nullptr)
                        {
                            if (auto mesh = ParseMeshAsset(AsStringView(*blob), assetPath))
                            {
                                meshRenderer.mesh = renderer.CreateMesh(mesh->vertices, mesh->indices);
                                meshCache.emplace(id.value, meshRenderer.mesh);
                            }
                        }
                        else
                        {
                            MIR_LOG_ERROR("ResolveSceneMeshes: could not load '{}'", assetPath);
                        }
                    }
                }

                if (meshRenderer.material.IsNull() && meshRenderer.materialShader[0] != '\0')
                {
                    const std::string shaderName = meshRenderer.materialShader;
                    if (auto it = materialCache.find(shaderName); it != materialCache.end())
                        meshRenderer.material = it->second;
                    else
                        meshRenderer.material = materialCache
                                                    .emplace(shaderName, renderer.CreateMaterial(
                                                                             MaterialDesc{.shaderName = shaderName}))
                                                    .first->second;
                }
            });
    }
}
