/**
 * @file TerrainRender.h
 * @author Jackey Cen
 * @brief Uploads terrain tile meshes as draws placed in the Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Mesh.h>
#include <renderer/MeshStreaming.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainTileMesh.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mir
{
    // P1 diagnostic look: the default material times this tint.
    inline constexpr glm::vec4 kTerrainDiagnosticTint{0.62f, 0.78f, 0.52f, 1.0f};

    // Lets neighbouring tiles meet bit for bit near the camera.
    inline constexpr float kTerrainTranslationGrid = 1.0f / 4096.0f;

    // In the mesh's own frame; color only feeds the unlit shader.
    std::vector<Vertex> MakeTerrainVertices(std::span<const TerrainMeshVertex> mesh,
                                            const glm::vec3 &color = glm::vec3(1.0f));

    // Terrain meshes through the renderer's asynchronous uploads; an id is a mesh handle.
    class TerrainGpuUploader final : public TerrainMeshUploader
    {
    public:
        explicit TerrainGpuUploader(VulkanRenderer &renderer) : mRenderer(renderer) {}

        // For meshes queued from now on.
        void SetRayTracing(bool rayTracing) { mOptions.rayTracing = rayTracing; }
        bool RayTracing() const { return mOptions.rayTracing; }

        uint64_t Footprint(uint32_t vertices, uint32_t indices) const override;
        uint64_t QueuedBytes() const override;
        TerrainGpuMeshId Upload(TerrainMeshData &&mesh) override;
        TerrainGpuMeshState State(TerrainGpuMeshId mesh) const override;
        void Retire(TerrainGpuMeshId mesh) override;
        uint64_t RetiringBytes() const override;

        static MeshHandle Handle(TerrainGpuMeshId mesh);

    private:
        VulkanRenderer &mRenderer;
        MeshUploadOptions mOptions;
    };

    // A ready mesh drawn at an instance's placement; nullopt when its origin does not fit the world.
    std::optional<WorldDrawItem> MakeTerrainDraw(const TerrainResidentMesh &mesh, const TerrainPlacement &placement);
}
