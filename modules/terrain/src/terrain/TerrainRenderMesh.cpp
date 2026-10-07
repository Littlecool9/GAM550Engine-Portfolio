#include <terrain/TerrainRenderMesh.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        TerrainResult<TerrainTileMesh> Refuse(std::string message)
        {
            return {std::nullopt, {TerrainPackErrorCode::InvalidInput, std::move(message)}};
        }
    }

    TerrainResult<TerrainTileMesh> MakeTerrainRenderMesh(const TerrainIndex &index, const TerrainLodSurface &lod,
                                                        const TerrainLodNormals &normals)
    {
        const TerrainTileRecord *source = index.Find(lod.tile);
        if (!source)
            return Refuse("the tile has no final source");
        const auto expectedOrigin = TerrainLocalPosition::FromMeters({
            index.layout.SamplePosition(index.layout.TileFirstSample(lod.tile.x)), 0.0,
            index.layout.SamplePosition(index.layout.TileFirstSample(lod.tile.z))});
        if (lod.release != index.release || normals.release != index.release ||
            lod.tile != normals.tile || lod.layer != normals.layer || lod.level != normals.level ||
            lod.layer >= index.layers.size() || lod.sourceChecksum != source->checksum ||
            lod.layer < source->layer || lod.level != lod.layer - source->layer ||
            !expectedOrigin || lod.origin.Meters() != expectedOrigin->Meters() ||
            normals.sourceChecksum != source->checksum || lod.sourceSpacing != source->sampleSpacing ||
            normals.sourceSpacing != source->sampleSpacing || lod.spacing != normals.spacing ||
            lod.spacing != index.layers[lod.layer].SampleSpacing() || lod.spacing < source->sampleSpacing ||
            lod.topologyHash != normals.topologyHash || lod.positions.size() != normals.normals.size() ||
            lod.positions.empty() || lod.indices.empty() || lod.indices.size() % 3 != 0 ||
            lod.topologyHash != TerrainTopologyHash(lod.positions, lod.indices))
            return Refuse("LOD, normals and final source have different provenance or topology");

        const TerrainTileRecord *record = index.Find(lod.tile, lod.layer, TerrainPayloadKind::LodHeight, lod.level);
        const TerrainTileRecord *normalRecord = index.Find(lod.tile, lod.layer, TerrainPayloadKind::Normal, lod.level);
        if (!record || !normalRecord || record->topologyHash != lod.topologyHash ||
            normalRecord->topologyHash != lod.topologyHash || record->sourceChecksum != lod.sourceChecksum ||
            normalRecord->sourceChecksum != lod.sourceChecksum || record->vertexCount != lod.positions.size() ||
            normalRecord->vertexCount != normals.normals.size() || record->indexCount != lod.indices.size() ||
            !record->maxDeviation || lod.maxDeviation != *record->maxDeviation)
            return Refuse("LOD or normals are not the indexed derivatives");
        if (Fnv1a64(EncodeTerrainLod(lod)) != record->checksum ||
            Fnv1a64(EncodeTerrainNormals(lod, normals.normals)) != normalRecord->checksum)
            return Refuse("decoded LOD or normal order differs from its indexed payload");

        TerrainTileMesh mesh;
        mesh.release = index.release;
        mesh.tile = lod.tile;
        mesh.origin = lod.origin;
        mesh.samples = {{1, 1}, {0, 0}};
        mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        mesh.vertices.reserve(lod.positions.size());
        for (size_t i = 0; i < lod.positions.size(); ++i)
        {
            const glm::vec3 p = lod.positions[i];
            const glm::vec3 n = normals.normals[i];
            const double length = glm::dot(glm::dvec3(n), glm::dvec3(n));
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                !std::isfinite(length) || std::abs(length - 1.0) > 1e-5)
                return Refuse("LOD positions or normals are invalid");
            mesh.vertices.push_back({p, n});
            mesh.boundsMin = glm::min(mesh.boundsMin, p);
            mesh.boundsMax = glm::max(mesh.boundsMax, p);
        }
        for (uint32_t i : lod.indices)
            if (i >= mesh.vertices.size())
                return Refuse("LOD index is outside its vertices");
        mesh.indices = lod.indices;
        return {std::move(mesh), {}};
    }

    TerrainResult<TerrainTileMesh> ReadTerrainRenderMesh(const TerrainReleaseHandle &release, TileCoord tile,
                                                        std::optional<uint32_t> layer)
    {
        const TerrainTileRecord *source = release.Index().Find(tile);
        if (!source)
            return Refuse("the tile has no final source");
        const uint32_t selected = layer.value_or(source->layer);
        const TileKey key{release.Info().asset, tile};
        auto lod = release.ReadLod(key, selected);
        if (!lod)
            return {std::nullopt, std::move(lod.error)};
        auto normals = release.ReadNormals(key, selected);
        if (!normals)
            return {std::nullopt, std::move(normals.error)};
        return MakeTerrainRenderMesh(release.Index(), *lod.value, *normals.value);
    }
}
