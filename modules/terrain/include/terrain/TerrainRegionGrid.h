/**
 * @file TerrainRegionGrid.h
 * @author Jackey Cen
 * @brief The Region quadtree over a release's reference grid: levels, addresses, extents and grid lines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLayout.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kMaxTerrainRegionCells = 4096;

    // x and z count from the first Region of that level, which every level shares.
    struct TerrainRegionCoord
    {
        uint32_t level = 0;
        int32_t x = 0;
        int32_t z = 0;

        constexpr bool operator==(const TerrainRegionCoord &) const = default;
    };

    // Level 0 Regions are squares of RegionTiles() tiles on the grid of that size through tile 0, so they
    // line up with chunks of that size. Level L + 1 Regions join 2 x 2 level L Regions, starting from the
    // map's first level 0 Region, until one Region covers the map. Grid lines run Cells() per side on
    // multiples of Unit() reference intervals.
    class TerrainRegionGrid
    {
    public:
        // Refuses a unit that does not divide the tile, and Regions past int32 tiles or 2^53 samples.
        [[nodiscard]] static std::optional<TerrainRegionGrid> Create(const TerrainLayout &reference,
                                                                     uint32_t regionTiles, uint32_t cells,
                                                                     uint32_t unit);

        const TerrainLayout &Reference() const { return mReference; }
        uint32_t RegionTiles() const { return mRegionTiles; }
        uint32_t Cells() const { return mCells; }
        uint32_t Unit() const { return mUnit; }
        uint32_t Levels() const { return static_cast<uint32_t>(mCounts.size()); }
        TileCoord FirstTile() const { return mFirstTile; }
        // Regions per axis.
        TileCoord Count(uint32_t level) const { return mCounts[level]; }
        uint64_t RegionCount() const { return mOrdinals.back(); }
        bool Contains(TerrainRegionCoord region) const;
        // Position in (level, z, x) order; region must be contained.
        uint64_t Ordinal(TerrainRegionCoord region) const;
        TerrainRegionCoord AtOrdinal(uint64_t ordinal) const;

        int64_t EdgeTiles(uint32_t level) const { return int64_t(mRegionTiles) << level; }
        // May reach past the map.
        TileRange Tiles(TerrainRegionCoord region) const;
        TileRange MapTiles(TerrainRegionCoord region) const;
        TerrainLocalBounds Rect(TerrainRegionCoord region) const;
        TerrainLocalBounds ValidExtent(TerrainRegionCoord region) const;
        // Reference samples of the area the Region meshes: its rect cut to the cells touching the bounds.
        SampleRange Domain(TerrainRegionCoord region) const;
        // Strictly increasing reference sample indices across the domain, both ends included.
        std::vector<int64_t> LinesX(TerrainRegionCoord region) const;
        std::vector<int64_t> LinesZ(TerrainRegionCoord region) const;
        // The widest cell of the level.
        double CellSpacing(uint32_t level) const;
        // Grid nodes per side of a Region the map does not clip.
        int32_t SamplesPerSide(uint32_t level) const;

        TerrainRegionCoord Containing(uint32_t level, TileCoord tile) const;
        std::optional<TerrainRegionCoord> Parent(TerrainRegionCoord region) const;
        std::vector<TerrainRegionCoord> Children(TerrainRegionCoord region) const;

        bool SameGrid(const TerrainRegionGrid &other) const;

    private:
        TerrainRegionGrid(const TerrainLayout &reference) : mReference(reference) {}

        int64_t FirstSampleX(TerrainRegionCoord region) const;
        int64_t FirstSampleZ(TerrainRegionCoord region) const;
        std::vector<int64_t> Lines(int64_t first, uint32_t level, int64_t domainMin, int64_t domainMax) const;

        TerrainLayout mReference;
        uint32_t mRegionTiles = 0;
        uint32_t mCells = 0;
        uint32_t mUnit = 0;
        TileCoord mFirstTile;
        // The cells of the reference grid that touch the bounds, as sample indices.
        SampleRange mMeshed;
        std::vector<TileCoord> mCounts;
        // Regions before each level; one past the last level is the total.
        std::vector<uint64_t> mOrdinals;
    };
}

template <>
struct std::hash<mir::TerrainRegionCoord>
{
    size_t operator()(const mir::TerrainRegionCoord &region) const noexcept
    {
        const uint64_t packed = (uint64_t(uint32_t(region.x)) << 32) | uint32_t(region.z);
        return std::hash<uint64_t>{}(packed) ^ (std::hash<uint32_t>{}(region.level) * 0x9e3779b97f4a7c15ull);
    }
};
