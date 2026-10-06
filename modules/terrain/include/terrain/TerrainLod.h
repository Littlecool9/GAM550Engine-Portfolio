#pragma once

#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainSurface.h>
#include <glm/vec3.hpp>
#include <span>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kTerrainLodContentVersion = 1;
    inline constexpr uint32_t kTerrainDerivedHeaderSize = 80;

    struct TerrainLodSurface
    {
        TerrainReleaseInfo release;
        TileCoord tile;
        uint32_t layer = 0;
        uint32_t level = 0;
        double sourceSpacing = 0.0;
        double spacing = 0.0;
        uint64_t sourceChecksum = 0;
        uint64_t topologyHash = 0;
        double maxDeviation = 0.0;
        TerrainLocalPosition origin;
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> indices;
    };

    struct TerrainLodNormals
    {
        TerrainReleaseInfo release;
        TileCoord tile;
        uint32_t layer = 0;
        uint32_t level = 0;
        double sourceSpacing = 0.0;
        double spacing = 0.0;
        uint64_t sourceChecksum = 0;
        uint64_t topologyHash = 0;
        std::vector<glm::vec3> normals;
    };

    uint64_t TerrainTopologyHash(std::span<const glm::vec3> positions, std::span<const uint32_t> indices);
    uint64_t TerrainFinalHeightChecksum(const TileHeights &tile);
    std::vector<std::byte> EncodeTerrainLod(const TerrainLodSurface &lod);
    std::vector<std::byte> EncodeTerrainNormals(const TerrainLodSurface &lod, std::span<const glm::vec3> normals);
    TerrainResult<TerrainLodSurface> DecodeTerrainLod(const TerrainIndex &index, const TerrainTileRecord &record,
                                                     std::span<const std::byte> payload);
    TerrainResult<TerrainLodNormals> DecodeTerrainNormals(const TerrainIndex &index, const TerrainTileRecord &record,
                                                         std::span<const std::byte> payload);
    // The actual decoded mesh; points are tile-local x/z meters.
    std::optional<double> EvaluateTerrainLod(const TerrainLodSurface &lod, double x, double z);
    std::optional<double> TerrainRegularDeviation(const TerrainLayout &sourceLayout, const TileHeights &source,
        const TerrainLayout &queryLayout, const TileHeights &query);
    // Exact XY triangle overlay extrema, with a numerical enclosure.
    std::optional<double> TerrainLodDeviation(const TerrainLayout &sourceLayout, const TileHeights &source,
                                             const TerrainLodSurface &lod);
}
