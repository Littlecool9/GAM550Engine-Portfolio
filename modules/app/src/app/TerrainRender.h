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
#include <renderer/VulkanRenderer.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainTileMesh.h>
#include <terrain/TerrainViewSelection.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    // P1 diagnostic look: the default material times this tint.
    inline constexpr glm::vec4 kTerrainDiagnosticTint{0.62f, 0.78f, 0.52f, 1.0f};

    // Lets neighbouring tiles meet bit for bit near the camera.
    inline constexpr float kTerrainTranslationGrid = 1.0f / 4096.0f;

    // In the mesh's own frame; color only feeds the unlit shader.
    std::vector<Vertex> MakeTerrainVertices(const TerrainTileMesh &mesh, const glm::vec3 &color = glm::vec3(1.0f));
    std::vector<Vertex> MakeTerrainVertices(const TerrainRegionMesh &mesh, const glm::vec3 &color = glm::vec3(1.0f));

    // GPU meshes of terrain draw units, made from resident payloads when first drawn and kept until unused for a
    // while. Every viewport and every instance of a release shares them.
    class TerrainMeshCache
    {
    public:
        struct Mesh
        {
            MeshHandle handle;
            // Terrain-local origin of the vertex frame.
            TerrainLocalPosition origin;
            uint32_t vertices = 0;
            double lastUsed = 0.0;
        };

        struct Counters
        {
            uint64_t vertices = 0;
            uint64_t created = 0;
            uint64_t retired = 0;
            uint64_t failed = 0;
        };

        bool Has(const TerrainPayloadKey &geometry) const { return mMeshes.contains(geometry); }
        bool Failed(const TerrainPayloadKey &geometry) const { return mFailed.contains(geometry); }
        size_t Size() const { return mMeshes.size(); }

        // Uploads the mesh from the streamer's resident payloads the first time. Null while they are not
        // resident, and for good once they do not make a mesh, which error then says.
        const Mesh *Acquire(VulkanRenderer &renderer, const TerrainStreamer &streamer, const TerrainIndex &index,
                            const TerrainDrawUnit &unit, double now, std::string &error);

        // At most once per retainSeconds, destroys the meshes unused that long, in one device wait.
        void Retire(VulkanRenderer &renderer, double now, double retainSeconds);
        // Destroys every mesh of a release not in keep, in one device wait.
        void RetireReleases(VulkanRenderer &renderer, std::span<const TerrainReleaseInfo> keep);
        // Call when ReleaseSceneResources runs: a kept handle would name the next scene's mesh.
        void Forget();

        const Counters &Stats() const { return mCounters; }

    private:
        void Destroy(VulkanRenderer &renderer, std::vector<MeshHandle> &meshes);

        std::unordered_map<TerrainPayloadKey, Mesh> mMeshes;
        std::unordered_set<TerrainPayloadKey> mFailed;
        double mLastRetire = -std::numeric_limits<double>::infinity();
        Counters mCounters;
    };

    // A cached mesh drawn at an instance's placement; nullopt when its origin does not fit the world.
    std::optional<WorldDrawItem> MakeTerrainDraw(const TerrainMeshCache::Mesh &mesh, const TerrainPlacement &placement);
}
