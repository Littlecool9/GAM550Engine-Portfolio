/**
 * @file TerrainTileMesh.cpp
 * @author Jackey Cen
 * @brief A tile's fixed-LOD triangle mesh built on the CPU from one release's final samples.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainTileMesh.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace mir
{
    namespace
    {
        struct CellSpan
        {
            int64_t first = 0;
            int64_t last = 0;
        };

        // Cells touching [lo, hi]; a bound on a sample adds no zero-width cell.
        CellSpan CellsTouching(const TerrainLayout &layout, int64_t tileFirst, double lo, double hi)
        {
            const int64_t tileLast = tileFirst + layout.IntervalsPerTile() - 1;
            CellSpan span{tileFirst, tileLast};
            while (span.first < tileLast && layout.SamplePosition(span.first + 1) <= lo)
                ++span.first;
            while (span.last > span.first && layout.SamplePosition(span.last) >= hi)
                --span.last;
            return span;
        }

        // Reads one step past an edge from that neighbour, fetched on first use.
        class SampleReader
        {
        public:
            SampleReader(const TerrainLayout &layout, TileCoord tile, std::span<const float> own,
                         const TileSampleLookup &lookup)
                : mLayout(layout), mTile(tile), mLookup(lookup), mSide(int64_t(layout.IntervalsPerTile()) + 1),
                  mI0(layout.TileFirstSample(tile.x)), mK0(layout.TileFirstSample(tile.z))
            {
                mBlocks[0] = own;
                mFetched[0] = true;
            }

            TerrainMeshStatus Status() const { return mStatus; }
            TileCoord FailedTile() const { return mFailedTile; }

        // At most one step past the tile, on one axis.
            bool Read(GlobalSampleIndex sample, float &out)
            {
                const int32_t dx = sample.x < mI0 ? -1 : (sample.x > mI0 + mSide - 1 ? 1 : 0);
                const int32_t dz = sample.z < mK0 ? -1 : (sample.z > mK0 + mSide - 1 ? 1 : 0);
                const TileCoord tile{mTile.x + dx, mTile.z + dz};
                const std::span<const float> block = Block(dx, dz, tile);
                if (block.empty())
                    return false;

                const int64_t i = sample.x - mLayout.TileFirstSample(tile.x);
                const int64_t k = sample.z - mLayout.TileFirstSample(tile.z);
                out = block[static_cast<size_t>(k * mSide + i)];
                if (!std::isfinite(out))
                    return Fail(TerrainMeshStatus::InvalidSamples, tile);
                return true;
            }

        // Past the layout, the sample itself.
            GlobalSampleIndex Step(GlobalSampleIndex sample, int32_t dx, int32_t dz) const
            {
                const GlobalSampleIndex next{sample.x + dx, sample.z + dz};
                const bool leaves = next.x < mI0 || next.x > mI0 + mSide - 1 || next.z < mK0 ||
                                    next.z > mK0 + mSide - 1;
                if (leaves && !mLayout.Contains(TileCoord{mTile.x + dx, mTile.z + dz}))
                    return sample;
                return next;
            }

        private:
            static size_t Slot(int32_t dx, int32_t dz)
            {
                if (dx != 0)
                    return dx < 0 ? 1 : 2;
                if (dz != 0)
                    return dz < 0 ? 3 : 4;
                return 0;
            }

            bool Fail(TerrainMeshStatus status, TileCoord tile)
            {
                mStatus = status;
                mFailedTile = tile;
                return false;
            }

            std::span<const float> Block(int32_t dx, int32_t dz, TileCoord tile)
            {
                const size_t slot = Slot(dx, dz);
                if (!mFetched[slot])
                {
                    mBlocks[slot] = mLookup(tile);
                    mFetched[slot] = true;
                }
                const std::span<const float> block = mBlocks[slot];
                if (block.empty())
                {
                    Fail(slot == 0 ? TerrainMeshStatus::MissingTile : TerrainMeshStatus::MissingNeighbour, tile);
                    return {};
                }
                if (block.size() != static_cast<size_t>(mSide * mSide))
                {
                    Fail(TerrainMeshStatus::InvalidSamples, tile);
                    return {};
                }
                return block;
            }

            const TerrainLayout &mLayout;
            TileCoord mTile;
            const TileSampleLookup &mLookup;
            int64_t mSide = 0;
            int64_t mI0 = 0;
            int64_t mK0 = 0;
            std::array<std::span<const float>, 5> mBlocks{};
            std::array<bool, 5> mFetched{};
            TerrainMeshStatus mStatus = TerrainMeshStatus::Ok;
            TileCoord mFailedTile;
        };

        // Global indices and sample values only, so every tile computes the same bits.
        bool CentralNormal(const TerrainLayout &layout, SampleReader &reader, GlobalSampleIndex sample,
                           glm::vec3 &out)
        {
            const GlobalSampleIndex left = reader.Step(sample, -1, 0);
            const GlobalSampleIndex right = reader.Step(sample, 1, 0);
            const GlobalSampleIndex down = reader.Step(sample, 0, -1);
            const GlobalSampleIndex up = reader.Step(sample, 0, 1);

            float hl = 0.0f, hr = 0.0f, hd = 0.0f, hu = 0.0f;
            if (!reader.Read(left, hl) || !reader.Read(right, hr) || !reader.Read(down, hd) || !reader.Read(up, hu))
                return false;

            const double gx = (double(hr) - double(hl)) / (layout.SamplePosition(right.x) - layout.SamplePosition(left.x));
            const double gz = (double(hu) - double(hd)) / (layout.SamplePosition(up.z) - layout.SamplePosition(down.z));
            out = glm::vec3(glm::normalize(glm::dvec3(-gx, 1.0, -gz)));
            return true;
        }
    }

    TerrainMeshResult BuildTerrainTileMesh(const TerrainLayout &layout, const TerrainReleaseInfo &release,
                                           TileCoord tile, const TileSampleLookup &lookup)
    {
        TerrainMeshResult result;
        result.tile = tile;
        if (!lookup || !layout.Contains(tile))
            return result;

        const int64_t side = int64_t(layout.IntervalsPerTile()) + 1;
        const std::span<const float> own = lookup(tile);
        if (own.empty())
        {
            result.status = TerrainMeshStatus::MissingTile;
            return result;
        }
        if (own.size() != static_cast<size_t>(side * side))
        {
            result.status = TerrainMeshStatus::InvalidSamples;
            return result;
        }

        const int64_t i0 = layout.TileFirstSample(tile.x);
        const int64_t k0 = layout.TileFirstSample(tile.z);
        const std::optional<TerrainLocalPosition> origin =
            TerrainLocalPosition::FromMeters({layout.SamplePosition(i0), 0.0, layout.SamplePosition(k0)});
        if (!origin)
            return result;

        const TerrainLocalBounds extent = *layout.ValidExtent(tile);
        const CellSpan cellsX = CellsTouching(layout, i0, extent.minX, extent.maxX);
        const CellSpan cellsZ = CellsTouching(layout, k0, extent.minZ, extent.maxZ);

        TerrainTileMesh mesh;
        mesh.release = release;
        mesh.tile = tile;
        mesh.origin = *origin;
        mesh.samples = {{cellsX.first, cellsZ.first}, {cellsX.last + 1, cellsZ.last + 1}};

        const int64_t columns = mesh.samples.max.x - mesh.samples.min.x + 1;
        const int64_t rows = mesh.samples.max.z - mesh.samples.min.z + 1;
        mesh.vertices.reserve(static_cast<size_t>(columns * rows));

        SampleReader reader(layout, tile, own, lookup);
        for (int64_t k = mesh.samples.min.z; k <= mesh.samples.max.z; ++k)
            for (int64_t i = mesh.samples.min.x; i <= mesh.samples.max.x; ++i)
            {
                TerrainMeshVertex vertex;
                if (!reader.Read({i, k}, vertex.position.y) || !CentralNormal(layout, reader, {i, k}, vertex.normal))
                {
                    result.status = reader.Status();
                    result.tile = reader.FailedTile();
                    return result;
                }
                vertex.position.x = static_cast<float>(layout.SamplePosition(i) - layout.SamplePosition(i0));
                vertex.position.z = static_cast<float>(layout.SamplePosition(k) - layout.SamplePosition(k0));
                mesh.vertices.push_back(vertex);
            }

        const auto vertexIndex = [&](GlobalSampleIndex sample)
        {
            return static_cast<uint32_t>((sample.z - mesh.samples.min.z) * columns + (sample.x - mesh.samples.min.x));
        };
        mesh.indices.reserve(static_cast<size_t>((columns - 1) * (rows - 1) * 6));
        for (int64_t k = cellsZ.first; k <= cellsZ.last; ++k)
            for (int64_t i = cellsX.first; i <= cellsX.last; ++i)
                for (const std::array<GlobalSampleIndex, 3> &triangle : TriangulateCell({i, k}).triangles)
                    for (const GlobalSampleIndex corner : triangle)
                        mesh.indices.push_back(vertexIndex(corner));

        mesh.boundsMin = mesh.boundsMax = mesh.vertices.front().position;
        for (const TerrainMeshVertex &vertex : mesh.vertices)
        {
            mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
            mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
        }

        result.status = TerrainMeshStatus::Ok;
        result.mesh = std::move(mesh);
        return result;
    }

    std::optional<WorldPosition> TerrainTileMeshWorldOrigin(const TerrainTileMesh &mesh,
                                                            const TerrainPlacement &placement)
    {
        if (placement.asset != mesh.release.asset)
            return std::nullopt;
        return TryTerrainLocalToWorld(mesh.origin, placement.translation);
    }
}
