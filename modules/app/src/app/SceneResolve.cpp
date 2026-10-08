/**
 * @file SceneResolve.cpp
 * @author Jackey Cen
 * @brief Post-load passes: saved names to live script instances and GPU handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SceneResolve.h"

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/components/MeshBounds.h>
#include <renderer/components/MeshRenderer.h>
#include <script/IScriptHost.h>
#include <script/ScriptReloadWatcher.h>
#include <script/components/ScriptRef.h>

#include <serial/JsonFile.h>

#include <glm/common.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        // Loads a cooked .lua and tracks it for hot reload; false if unavailable (already logged)
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

        // MeshData from a mesh JSON asset; nullopt (logged) if malformed
        std::optional<MeshData> ParseMeshAsset(std::string_view text, std::string_view assetPath)
        {
            const std::optional<nlohmann::json> json = ParseJson(text, assetPath);
            if (!json)
                return std::nullopt;

            MeshData mesh;
            for (const auto &v : json->value("vertices", nlohmann::json::array()))
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
            mesh.indices = json->value("indices", std::vector<uint32_t>{});
            return mesh;
        }

        MeshBounds ComputeMeshBounds(std::span<const Vertex> vertices)
        {
            glm::vec3 min(std::numeric_limits<float>::max());
            glm::vec3 max(std::numeric_limits<float>::lowest());

            for (const Vertex &vertex : vertices)
            {
                min = glm::min(min, vertex.pos);
                max = glm::max(max, vertex.pos);
            }

            if (vertices.empty())
                min = max = glm::vec3(0.0f);

            return MeshBounds{ min, max };
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
                const AssetPathResult assetPath = MakeLogicalAssetPath(assetIdRoot, "scripts/" + name + ".lua");
                if (!assetPath.Ok())
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: script '{}': {}", name, Describe(assetPath.error));
                    return;
                }

                if (!LoadScriptAsset(scripts, reload, assets, assetPath.path, name))
                {
                    MIR_LOG_ERROR("ResolveSceneScripts: could not load '{}' for script '{}'", assetPath.path, name);
                    return;
                }

                ref.instanceRef = scripts.CreateInstance(name);
            });
    }

    void ResolveSceneMeshes(World &world, VulkanRenderer &renderer, AssetCache *assets,
        std::string_view assetIdRoot)
    {
        std::unordered_map<uint64_t, MeshHandle> meshCache;
        std::unordered_map<uint64_t, MeshBounds> meshBoundsCache;
        std::unordered_map<std::string, MaterialHandle> materialCache;
        std::vector<std::pair<Entity, MeshBounds>> pendingBounds;

        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity entity, MeshRenderer &meshRenderer)
            {
                AssetPathResult logical;
                if (meshRenderer.mesh.IsNull() && meshRenderer.meshName[0] != '\0')
                {
                    logical = MakeLogicalAssetPath(assetIdRoot, meshRenderer.meshName);
                    if (!logical.Ok())
                        MIR_LOG_ERROR("ResolveSceneMeshes: mesh '{}': {}", meshRenderer.meshName,
                                      Describe(logical.error));
                }

                if (logical.Ok() && !logical.path.empty())
                {
                    const std::string &assetPath = logical.path;
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
                                meshBoundsCache.emplace(id.value, ComputeMeshBounds(mesh->vertices));
                            }
                        }
                        else
                        {
                            MIR_LOG_ERROR("ResolveSceneMeshes: could not load '{}'", assetPath);
                        }
                    }

                    if (auto it = meshBoundsCache.find(id.value); it != meshBoundsCache.end())
                        pendingBounds.emplace_back(entity, it->second);
                }

                if (meshRenderer.material.IsNull() && meshRenderer.materialShader[0] != '\0')
                {
                    const std::string shaderName = meshRenderer.materialShader;
                    if (auto it = materialCache.find(shaderName); it != materialCache.end())
                        meshRenderer.material = it->second;
                    else
                    {
                        // Null would draw the default grey, which hides that the authored shader failed.
                        MaterialHandle material = renderer.CreateMaterial(MaterialDesc{ .shaderName = shaderName });
                        if (material.IsNull())
                        {
                            MIR_LOG_ERROR("ResolveSceneMeshes: material '{}' failed, using error material", shaderName);
                            material = renderer.ErrorMaterial();
                        }
                        meshRenderer.material = materialCache.emplace(shaderName, material).first->second;
                    }
                }
            });

        for (const auto &[entity, bounds] : pendingBounds)
            world.AddComponent<MeshBounds>(entity, bounds);
    }
}
