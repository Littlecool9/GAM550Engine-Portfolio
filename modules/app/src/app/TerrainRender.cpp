/**
 * @file TerrainRender.cpp
 * @author Jackey Cen
 * @brief Uploads terrain tile meshes as raster-only draws placed in the Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainRender.h"

#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <renderer/RenderSystem.h>

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
        const MeshHandle handle = renderer.CreateMesh(vertices, mesh.indices, MeshUsage::RasterOnly);
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
        mRelease = mesh.release;
        return true;
    }

    void TerrainRenderTiles::Destroy(VulkanRenderer &renderer)
    {
        for (const WorldDrawItem &draw : mDraws)
            renderer.DestroyMesh(draw.mesh);
        Forget();
    }

    void TerrainRenderTiles::Forget()
    {
        mDraws.clear();
        mRelease.reset();
    }

    void PublishTerrainDraws(World &world, RenderSystem &renderSystem, std::span<const WorldDrawItem> draws)
    {
        TerrainDrawItems *resource = world.TryResource<TerrainDrawItems>();
        if (!resource)
            resource = &world.EmplaceResource<TerrainDrawItems>();
        resource->items.assign(draws.begin(), draws.end());
        renderSystem.SetForceRaster(!draws.empty());
    }
}
