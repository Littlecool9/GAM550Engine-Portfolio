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

#include <utility>

namespace mir
{
    std::vector<Vertex> MakeTerrainVertices(std::span<const TerrainMeshVertex> mesh, const glm::vec3 &color)
    {
        std::vector<Vertex> vertices;
        vertices.reserve(mesh.size());
        for (const TerrainMeshVertex &v : mesh)
            vertices.push_back(Vertex{.pos = v.position, .color = color, .normal = v.normal});
        return vertices;
    }

    uint64_t TerrainGpuUploader::Footprint(uint32_t vertices, uint32_t indices) const
    {
        return mRenderer.MeshFootprint(vertices, indices, mOptions);
    }

    uint64_t TerrainGpuUploader::QueuedBytes() const
    {
        return mRenderer.MeshStreaming().pendingBytes;
    }

    TerrainGpuMeshId TerrainGpuUploader::Upload(TerrainMeshData &&mesh)
    {
        const MeshHandle handle = mRenderer.QueueMesh(MakeTerrainVertices(mesh.vertices), std::move(mesh.indices),
                                                      mOptions);
        if (handle.IsNull())
            return 0;
        return (uint64_t(handle.mGeneration) << 32) | handle.mIndex;
    }

    TerrainGpuMeshState TerrainGpuUploader::State(TerrainGpuMeshId mesh) const
    {
        switch (mRenderer.MeshState(Handle(mesh)))
        {
        case MeshUploadState::Ready: return TerrainGpuMeshState::Ready;
        case MeshUploadState::Pending:
        case MeshUploadState::Uploading: return TerrainGpuMeshState::Uploading;
        case MeshUploadState::Stale:
        case MeshUploadState::Failed: return TerrainGpuMeshState::Failed;
        }
        return TerrainGpuMeshState::Failed;
    }

    void TerrainGpuUploader::Retire(TerrainGpuMeshId mesh)
    {
        mRenderer.RetireMesh(Handle(mesh));
    }

    uint64_t TerrainGpuUploader::RetiringBytes() const
    {
        return mRenderer.MeshStreaming().retiringBytes;
    }

    MeshHandle TerrainGpuUploader::Handle(TerrainGpuMeshId mesh)
    {
        return MeshHandle{uint32_t(mesh & 0xffffffffu), uint32_t(mesh >> 32)};
    }

    std::optional<WorldDrawItem> MakeTerrainDraw(const TerrainResidentMesh &mesh, const TerrainPlacement &placement)
    {
        const std::optional<WorldPosition> origin = TryTerrainLocalToWorld(mesh.origin, placement.translation);
        if (!origin)
            return std::nullopt;
        // Identity linear part keeps normals as built.
        WorldDrawItem draw;
        draw.mesh = TerrainGpuUploader::Handle(mesh.id);
        draw.pose = WorldPose{*origin};
        draw.tint = kTerrainDiagnosticTint;
        draw.translationGrid = kTerrainTranslationGrid;
        return draw;
    }
}
