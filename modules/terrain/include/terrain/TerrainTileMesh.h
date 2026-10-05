/**
 * @file TerrainTileMesh.h
 * @author Jackey Cen
 * @brief A tile's fixed-LOD triangle mesh built on the CPU from one release's final samples.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainSurface.h>
#include <terrain/TileKey.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mir
{
    struct TerrainMeshVertex
    {
        // Meters from the mesh origin; y is the stored sample unchanged.
        glm::vec3 position{0.0f};
        // Central differences; one-sided only past the layout.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
    };

    // Derived and rebuildable; never a second source of heights.
    struct TerrainTileMesh
    {
        TerrainReleaseInfo release;
        uint32_t surfaceVersion = kTerrainSurfaceVersion;
        TileCoord tile;
        // The tile's first sample at height 0, so vertices stay small in float.
        TerrainLocalPosition origin;
        // Inclusive; only the cells touching the map when bounds clip the tile.
        SampleRange samples;
        std::vector<TerrainMeshVertex> vertices;
        // As TriangulateCell orders and winds them.
        std::vector<uint32_t> indices;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };

    enum class TerrainMeshStatus : uint8_t
    {
        Ok,
        InvalidInput,
        MissingTile,
        // Edge normals need the neighbour's samples.
        MissingNeighbour,
        InvalidSamples,
    };

    struct TerrainMeshResult
    {
        TerrainMeshStatus status = TerrainMeshStatus::InvalidInput;
        // The tile whose samples are missing or bad.
        TileCoord tile;
        std::optional<TerrainTileMesh> mesh;

        bool IsOk() const { return status == TerrainMeshStatus::Ok; }
    };

    // Also reads the four edge neighbours (never diagonals), so both sides of an edge agree.
    TerrainMeshResult BuildTerrainTileMesh(const TerrainLayout &layout, const TerrainReleaseInfo &release,
                                           TileCoord tile, const TileSampleLookup &lookup);

    // nullopt for another asset or an origin that does not fit.
    std::optional<WorldPosition> TerrainTileMeshWorldOrigin(const TerrainTileMesh &mesh,
                                                            const TerrainPlacement &placement);
}
