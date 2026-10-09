/**
 * @file TerrainRender.cpp
 * @author Jackey Cen
 * @brief Uploads terrain tile meshes as draws placed in the Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRender.h"

#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>

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

    bool TerrainRenderTiles::Add(VulkanRenderer &renderer, const TerrainTileMesh &mesh,
                                 const TerrainPlacement &placement, MaterialHandle material, const glm::vec4 &tint)
    {
        if (mRelease && *mRelease != mesh.release)
        {
            MIR_LOG_ERROR("Terrain tile ({}, {}) is from release {:016x}, not {:016x}; not drawn", mesh.tile.x,
                          mesh.tile.z, mesh.release.release.value, mRelease->release.value);
            return false;
        }

        const std::optional<WorldPosition> origin = TerrainTileMeshWorldOrigin(mesh, placement);
        if (!origin)
        {
            MIR_LOG_ERROR("Terrain tile ({}, {}) cannot be placed: another asset, or past the world range",
                          mesh.tile.x, mesh.tile.z);
            return false;
        }

        const std::vector<Vertex> vertices = MakeTerrainVertices(mesh);
        const MeshHandle handle = renderer.CreateMesh(vertices, mesh.indices);
        if (handle.IsNull())
            return false;

        // Identity linear part keeps normals as built.
        WorldDrawItem draw;
        draw.mesh = handle;
        draw.pose = WorldPose{*origin};
        draw.tint = tint;
        draw.material = material;
        draw.translationGrid = kTerrainTranslationGrid;
        mDraws.push_back(draw);
        mOrigins.push_back(mesh.origin);
        mRelease = mesh.release;
        return true;
    }

    bool TerrainRenderTiles::Place(const TerrainPlacement &placement)
    {
        if (mRelease && mRelease->asset != placement.asset)
        {
            MIR_LOG_ERROR("Terrain tiles of asset {:016x} cannot take a placement of asset {:016x}",
                          mRelease->asset.value, placement.asset.value);
            return false;
        }

        std::vector<WorldPosition> origins;
        origins.reserve(mOrigins.size());
        for (const TerrainLocalPosition &local : mOrigins)
        {
            const std::optional<WorldPosition> origin = TryTerrainLocalToWorld(local, placement.translation);
            if (!origin)
            {
                MIR_LOG_ERROR("Terrain tiles cannot move past the world range");
                return false;
            }
            origins.push_back(*origin);
        }
        for (size_t i = 0; i < mDraws.size(); ++i)
            mDraws[i].pose = WorldPose{origins[i]};
        return true;
    }

    void TerrainRenderTiles::Destroy(VulkanRenderer &renderer)
    {
        std::vector<MeshHandle> meshes;
        TakeMeshes(meshes);
        if (!meshes.empty())
            renderer.ReleaseModelResources(meshes, {}, {});
    }

    void TerrainRenderTiles::TakeMeshes(std::vector<MeshHandle> &meshes)
    {
        for (const WorldDrawItem &draw : mDraws)
            meshes.push_back(draw.mesh);
        Forget();
    }

    void TerrainRenderTiles::Forget()
    {
        mDraws.clear();
        mOrigins.clear();
        mRelease.reset();
    }

    void PublishTerrainDraws(World &world, std::span<const WorldDrawItem> draws)
    {
        TerrainDrawItems *resource = world.TryResource<TerrainDrawItems>();
        if (!resource)
            resource = &world.EmplaceResource<TerrainDrawItems>();
        resource->items.assign(draws.begin(), draws.end());
    }
}
