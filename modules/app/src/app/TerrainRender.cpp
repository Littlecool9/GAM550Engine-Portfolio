/**
 * @file TerrainRender.cpp
 * @author Jackey Cen
 * @brief Uploads terrain tile meshes as draws placed in the Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRender.h"

#include <core/ecs/components/WorldTransform.h>
#include <terrain/TerrainRenderMesh.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace mir
{
    std::vector<Vertex> MakeTerrainVertices(const TerrainTileMesh &mesh, const glm::vec3 &color)
    {
        std::vector<Vertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const TerrainMeshVertex &v : mesh.vertices)
            vertices.push_back(Vertex{.pos = v.position, .color = color, .normal = v.normal});
        return vertices;
    }

    std::vector<Vertex> MakeTerrainVertices(const TerrainRegionMesh &mesh, const glm::vec3 &color)
    {
        std::vector<Vertex> vertices;
        vertices.reserve(mesh.positions.size());
        for (size_t i = 0; i < mesh.positions.size(); ++i)
            vertices.push_back(Vertex{.pos = mesh.positions[i], .color = color, .normal = mesh.normals[i]});
        return vertices;
    }

    const TerrainMeshCache::Mesh *TerrainMeshCache::Acquire(VulkanRenderer &renderer, const TerrainStreamer &streamer,
                                                            const TerrainIndex &index, const TerrainDrawUnit &unit,
                                                            double now, std::string &error)
    {
        if (const auto found = mMeshes.find(unit.geometry); found != mMeshes.end())
        {
            found->second.lastUsed = now;
            return &found->second;
        }
        if (mFailed.contains(unit.geometry))
            return nullptr;

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        TerrainLocalPosition origin;
        if (unit.kind == TerrainUnitKind::Region)
        {
            const std::shared_ptr<const TerrainRegionMesh> region = streamer.Region(unit.geometry);
            if (!region)
                return nullptr;
            vertices = MakeTerrainVertices(*region);
            indices = region->indices;
            origin = region->origin;
        }
        else
        {
            const std::shared_ptr<const TerrainLodSurface> lod = streamer.Lod(unit.geometry);
            const std::shared_ptr<const TerrainLodNormals> normals = streamer.Normals(unit.normals);
            if (!lod || !normals)
                return nullptr;
            TerrainResult<TerrainTileMesh> built = MakeTerrainRenderMesh(index, *lod, *normals);
            if (!built)
            {
                mFailed.insert(unit.geometry);
                ++mCounters.failed;
                error = built.error.message;
                return nullptr;
            }
            vertices = MakeTerrainVertices(*built.value);
            indices = std::move(built.value->indices);
            origin = built.value->origin;
        }

        const MeshHandle handle = renderer.CreateMesh(vertices, indices);
        if (handle.IsNull())
        {
            mFailed.insert(unit.geometry);
            ++mCounters.failed;
            error = "the GPU upload failed";
            return nullptr;
        }
        ++mCounters.created;
        mCounters.vertices += vertices.size();
        Mesh &mesh = mMeshes[unit.geometry];
        mesh = {handle, origin, uint32_t(vertices.size()), now};
        return &mesh;
    }

    void TerrainMeshCache::Retire(VulkanRenderer &renderer, double now, double retainSeconds)
    {
        if (now - mLastRetire < retainSeconds)
            return;
        mLastRetire = now;
        std::vector<MeshHandle> meshes;
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            if (now - it->second.lastUsed < retainSeconds)
            {
                ++it;
                continue;
            }
            meshes.push_back(it->second.handle);
            mCounters.vertices -= it->second.vertices;
            it = mMeshes.erase(it);
        }
        Destroy(renderer, meshes);
    }

    void TerrainMeshCache::RetireReleases(VulkanRenderer &renderer, std::span<const TerrainReleaseInfo> keep)
    {
        std::vector<MeshHandle> meshes;
        for (auto it = mMeshes.begin(); it != mMeshes.end();)
        {
            const TerrainPayloadKey &key = it->first;
            const bool kept = std::any_of(keep.begin(), keep.end(), [&](const TerrainReleaseInfo &release) {
                return release.asset == key.asset && release.release == key.release;
            });
            if (kept)
            {
                ++it;
                continue;
            }
            meshes.push_back(it->second.handle);
            mCounters.vertices -= it->second.vertices;
            it = mMeshes.erase(it);
        }
        std::erase_if(mFailed, [&](const TerrainPayloadKey &key) {
            return std::none_of(keep.begin(), keep.end(), [&](const TerrainReleaseInfo &release) {
                return release.asset == key.asset && release.release == key.release;
            });
        });
        Destroy(renderer, meshes);
    }

    void TerrainMeshCache::Forget()
    {
        mMeshes.clear();
        mFailed.clear();
        mCounters.vertices = 0;
    }

    void TerrainMeshCache::Destroy(VulkanRenderer &renderer, std::vector<MeshHandle> &meshes)
    {
        if (meshes.empty())
            return;
        mCounters.retired += meshes.size();
        renderer.ReleaseModelResources(meshes, {}, {});
    }

    std::optional<WorldDrawItem> MakeTerrainDraw(const TerrainMeshCache::Mesh &mesh, const TerrainPlacement &placement)
    {
        const std::optional<WorldPosition> origin = TryTerrainLocalToWorld(mesh.origin, placement.translation);
        if (!origin)
            return std::nullopt;
        // Identity linear part keeps normals as built.
        WorldDrawItem draw;
        draw.mesh = mesh.handle;
        draw.pose = WorldPose{*origin};
        draw.tint = kTerrainDiagnosticTint;
        draw.translationGrid = kTerrainTranslationGrid;
        return draw;
    }
}
