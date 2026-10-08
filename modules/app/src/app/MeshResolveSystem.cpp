/**
 * @file MeshResolveSystem.cpp
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MeshResolveSystem.h"

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/ecs/Query.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/components/MeshRenderer.h>

#include <serial/JsonFile.h>

#include <glm/common.hpp>

#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        // MeshData from a mesh JSON asset
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

    MeshResolveSystem::MeshResolveSystem(VulkanRenderer &renderer, AssetCache *assets, std::string assetIdRoot)
        : mRenderer(renderer), mAssets(assets), mAssetIdRoot(std::move(assetIdRoot))
    {}

    void MeshResolveSystem::Reset()
    {
        mMeshes.clear();
        mMaterials.clear();
        mResolved.clear();
    }

    const MeshResolveSystem::CachedMesh &MeshResolveSystem::ResolveMesh(const std::string &meshName)
    {
        if (const auto it = mMeshes.find(meshName); it != mMeshes.end())
            return it->second;

        CachedMesh &cached = mMeshes[meshName];

        const AssetPathResult logical = MakeLogicalAssetPath(mAssetIdRoot, meshName);
        if (!logical.Ok())
        {
            MIR_LOG_ERROR("MeshResolveSystem: mesh '{}': {}", meshName, Describe(logical.error));
            return cached;
        }

        const AssetBlobView *blob = mAssets != nullptr ? mAssets->Load(MakeAssetId(logical.path)) : nullptr;
        if (blob == nullptr)
        {
            MIR_LOG_ERROR("MeshResolveSystem: could not load '{}'", logical.path);
            return cached;
        }

        if (const std::optional<MeshData> mesh = ParseMeshAsset(AsStringView(*blob), logical.path))
        {
            cached.mMesh = mRenderer.CreateMesh(mesh->vertices, mesh->indices);
            cached.mBounds = ComputeMeshBounds(mesh->vertices);
        }
        return cached;
    }

    MaterialHandle MeshResolveSystem::ResolveMaterial(const std::string &shaderName)
    {
        if (const auto it = mMaterials.find(shaderName); it != mMaterials.end())
            return it->second;

        // Null would draw the default grey, which hides that the authored shader failed.
        MaterialHandle material = mRenderer.CreateMaterial(MaterialDesc{ .shaderName = shaderName });
        if (material.IsNull())
        {
            MIR_LOG_ERROR("MeshResolveSystem: material '{}' failed, using error material", shaderName);
            material = mRenderer.ErrorMaterial();
        }
        return mMaterials.emplace(shaderName, material).first->second;
    }

    void MeshResolveSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;

        std::vector<Entity> staleBounds;
        std::erase_if(mResolved,
            [&](const auto &entry)
            {
                const Entity entity = UnpackEntity(entry.first);
                if (world.IsAlive(entity) && world.Has<MeshRenderer>(entity))
                    return false;
                if (world.IsAlive(entity))
                    staleBounds.push_back(entity);
                return true;
            });

        std::vector<std::pair<Entity, std::optional<MeshBounds>>> pendingBounds;

        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity entity, MeshRenderer &meshRenderer)
            {
                ResolvedNames &resolved = mResolved[PackEntity(entity)];
                const std::string_view meshName = meshRenderer.meshName;
                const std::string_view shaderName = meshRenderer.materialShader;

                if (resolved.mMesh != meshName)
                {
                    resolved.mMesh = meshName;
                    const CachedMesh *cached = meshName.empty() ? nullptr : &ResolveMesh(resolved.mMesh);
                    meshRenderer.mesh = cached != nullptr ? cached->mMesh : MeshHandle{};
                    pendingBounds.emplace_back(entity, cached != nullptr ? cached->mBounds : std::nullopt);
                }

                if (resolved.mShader != shaderName)
                {
                    resolved.mShader = shaderName;
                    meshRenderer.material = shaderName.empty() ? MaterialHandle{} : ResolveMaterial(resolved.mShader);
                }
            });

        for (const Entity entity : staleBounds)
        {
            if (world.Has<MeshBounds>(entity))
                world.RemoveComponent<MeshBounds>(entity);
        }

        for (const auto &[entity, bounds] : pendingBounds)
        {
            if (bounds)
            {
                if (MeshBounds *existing = world.GetComponent<MeshBounds>(entity))
                    *existing = *bounds;
                else
                    world.AddComponent<MeshBounds>(entity, *bounds);
            }
            else if (world.Has<MeshBounds>(entity))
            {
                world.RemoveComponent<MeshBounds>(entity);
            }
        }
    }
}
