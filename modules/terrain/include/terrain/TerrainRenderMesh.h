#pragma once

#include <terrain/TerrainRelease.h>
#include <terrain/TerrainTileMesh.h>

namespace mir
{
    // Null selects the source layer; samples is empty for the boundary-ring mesh.
    TerrainResult<TerrainTileMesh> ReadTerrainRenderMesh(const TerrainReleaseHandle &release, TileCoord tile,
                                                        std::optional<uint32_t> layer = std::nullopt);
    TerrainResult<TerrainTileMesh> MakeTerrainRenderMesh(const TerrainIndex &index, const TerrainLodSurface &lod,
                                                        const TerrainLodNormals &normals);
}
