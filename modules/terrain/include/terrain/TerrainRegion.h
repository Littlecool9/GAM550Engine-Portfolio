/**
 * @file TerrainRegion.h
 * @author Jackey Cen
 * @brief Coarse Region mesh payloads: encoding, checked decoding, coverage and error against the final surface.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegionGrid.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kTerrainRegionContentVersion = 1;
    inline constexpr uint32_t kTerrainRegionHeaderSize = 112;

    // Positions are relative to origin, the first sample of the Region's rect. Triangles split each cell
    // along TriangulateCell's diagonal and winding; an outer edge inside the map also holds every node
    // of the finer source beside it, the nodes tile LODs keep on that edge.
    struct TerrainRegionMesh
    {
        TerrainReleaseInfo release;
        TerrainRegionCoord region;
        // The map tiles it is derived from.
        TileRange tiles;
        double cellSpacing = 0.0;
        // Fnv1a64 of (x, z, source checksum) of those tiles in (z, x) order.
        uint64_t sourceChecksum = 0;
        uint64_t topologyHash = 0;
        // Bounds |mesh - final surface| over the Region's valid extent.
        double maxDeviation = 0.0;
        TerrainLocalPosition origin;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<uint32_t> indices;
    };

    struct TerrainSourceChecksum
    {
        TileCoord tile;
        uint64_t checksum = 0;
    };

    // One value per Region of the level in (z, x) order; sources are the map tiles in (z, x) order.
    std::vector<uint64_t> TerrainRegionSourceChecksums(const TerrainRegionGrid &grid, uint32_t level,
                                                       std::span<const TerrainSourceChecksum> sources);

    // Header plus positions, normals and indices.
    uint64_t TerrainRegionPayloadLength(uint32_t vertexCount, uint32_t indexCount);
    std::vector<std::byte> EncodeTerrainRegion(const TerrainRegionMesh &mesh);
    // Checks the payload against its record and the index's grid: provenance, domain, winding, normals,
    // height range, topology and coverage.
    TerrainResult<TerrainRegionMesh> DecodeTerrainRegion(const TerrainIndex &index, const TerrainTileRecord &record,
                                                         std::span<const std::byte> payload);

    // The mesh at origin-relative x, z; nullopt off the mesh.
    std::optional<double> EvaluateTerrainRegion(const TerrainRegionMesh &mesh, double x, double z);

    // Every vertex lies in the domain and the triangle areas add up to the domain's.
    bool TerrainRegionCoversDomain(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh);

    struct TerrainSourceTile
    {
        const TerrainLayout *layout = nullptr;
        const TileHeights *heights = nullptr;
    };
    using TerrainSourceLookup = std::function<TerrainSourceTile(TileCoord tile)>;

    // Max |mesh - final source| over each map tile's valid extent, with TerrainLodDeviation's numerical
    // enclosure. Cells aligned with a tile's source grid compare at its nodes, others over the exact
    // triangle overlay. nullopt when a source is missing or a value cannot be enclosed.
    std::optional<double> TerrainRegionDeviation(const TerrainRegionGrid &grid, const TerrainRegionMesh &mesh,
                                                 const TerrainSourceLookup &sources);
}
