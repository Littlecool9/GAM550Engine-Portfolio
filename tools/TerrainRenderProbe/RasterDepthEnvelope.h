#pragma once
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainTileMesh.h>
#include <array>
#include <span>
#include <vector>

struct RasterDepthCell
{
    double minDepth = 1.0;
    double maxDepth = 0.0;
    double guaranteedMinDepth = 0.0;
    double maxDepthGradient = 0.0;
    size_t gradientTriangle = 0;
    uint32_t candidates = 0;
    double idealDepth = 0.0;
    double quantizedDepth = 0.0;
    uint32_t idealTriangle = UINT32_MAX;
    uint32_t quantizedTriangle = UINT32_MAX;
};
struct ProjectedTerrainTriangle
{
    std::array<glm::dvec3, 3> screen;
    std::array<glm::dvec3, 3> rasterViewport;
    glm::dvec2 depthGradient;
    mir::TileCoord tile;
    uint32_t primitive = 0;
};
struct RasterDepthEnvelope
{
    double quantum = 0.0;
    std::vector<RasterDepthCell> cells;
    std::vector<ProjectedTerrainTriangle> triangles;
};
RasterDepthEnvelope BuildRasterDepthEnvelope(std::span<const mir::TerrainTileMesh> meshes,
    std::span<const mir::DrawItem> items, const glm::mat4 &viewProj,
    uint32_t width, uint32_t height, uint32_t subPixelBits, const mir::MaterialDesc &material);
