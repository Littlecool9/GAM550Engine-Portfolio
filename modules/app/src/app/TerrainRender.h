/**
 * @file TerrainRender.h
 * @author Jackey Cen
 * @brief Uploads terrain tile meshes as draws placed in the Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainTileMesh.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <span>
#include <vector>

namespace mir
{
    class World;

    // P1 diagnostic look: the default material times this tint.
    inline constexpr glm::vec4 kTerrainDiagnosticTint{0.62f, 0.78f, 0.52f, 1.0f};

    // Lets neighbouring tiles meet bit for bit near the camera.
    inline constexpr float kTerrainTranslationGrid = 1.0f / 4096.0f;

    // In the mesh's own tile frame; color only feeds the unlit shader.
    std::vector<Vertex> MakeTerrainVertices(const TerrainTileMesh &mesh, const glm::vec3 &color = glm::vec3(1.0f));

    // One terrain instance's tile meshes and draws, all from one release.
    class TerrainRenderTiles
    {
    public:
        // False (logged) for another release or asset, an origin that does not fit, or a failed upload.
        bool Add(VulkanRenderer &renderer, const TerrainTileMesh &mesh, const TerrainPlacement &placement,
                 MaterialHandle material = kNullMaterial, const glm::vec4 &tint = kTerrainDiagnosticTint);

        std::span<const WorldDrawItem> DrawItems() const { return mDraws; }
        const std::optional<TerrainReleaseInfo> &Release() const { return mRelease; }
        bool Empty() const { return mDraws.empty(); }

        // Each DestroyMesh waits for the device.
        void Destroy(VulkanRenderer &renderer);

        // Call when ReleaseSceneResources runs: a kept handle would name the next scene's mesh.
        void Forget();

    private:
        std::optional<TerrainReleaseInfo> mRelease;
        std::vector<WorldDrawItem> mDraws;
    };

    // Publish an empty span before the meshes are freed.
    void PublishTerrainDraws(World &world, std::span<const WorldDrawItem> draws);
}
