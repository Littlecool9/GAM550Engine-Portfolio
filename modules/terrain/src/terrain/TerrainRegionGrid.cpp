/**
 * @file TerrainRegionGrid.cpp
 * @author Jackey Cen
 * @brief The Region quadtree over a release's reference grid: levels, addresses, extents and grid lines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainRegionGrid.h"

#include "AxisLocate.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace mir
{
    namespace
    {
        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            int64_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        constexpr int64_t kExactSampleLimit = int64_t(1) << 53;
    }

    std::optional<TerrainRegionGrid> TerrainRegionGrid::Create(const TerrainLayout &reference, uint32_t regionTiles,
                                                               uint32_t cells, uint32_t unit)
    {
        const int64_t n = reference.IntervalsPerTile();
        if (regionTiles < 1 || regionTiles > (1u << 20) || cells < 1 || cells > kMaxTerrainRegionCells ||
            unit < 1 || n % unit != 0)
            return std::nullopt;

        TerrainRegionGrid grid(reference);
        grid.mRegionTiles = regionTiles;
        grid.mCells = cells;
        grid.mUnit = unit;
        const TileRange tiles = reference.Tiles();
        const int64_t r = regionTiles;
        const int64_t firstX = FloorDiv(tiles.min.x, r), firstZ = FloorDiv(tiles.min.z, r);
        int64_t countX = FloorDiv(tiles.max.x, r) - firstX + 1;
        int64_t countZ = FloorDiv(tiles.max.z, r) - firstZ + 1;
        if (firstX * r < std::numeric_limits<int32_t>::min() || firstZ * r < std::numeric_limits<int32_t>::min())
            return std::nullopt;
        grid.mFirstTile = {static_cast<int32_t>(firstX * r), static_cast<int32_t>(firstZ * r)};

        grid.mOrdinals.push_back(0);
        for (uint32_t level = 0;; ++level)
        {
            if (level > 40)
                return std::nullopt;
            const int64_t edge = r << level;
            // Every Region's last tile and last sample stay addressable.
            const int64_t lastTileX = firstX * r + countX * edge, lastTileZ = firstZ * r + countZ * edge;
            if (lastTileX > std::numeric_limits<int32_t>::max() || lastTileZ > std::numeric_limits<int32_t>::max() ||
                std::max({std::abs(firstX * r), std::abs(firstZ * r), lastTileX, lastTileZ}) >= kExactSampleLimit / n)
                return std::nullopt;
            grid.mCounts.push_back({static_cast<int32_t>(countX), static_cast<int32_t>(countZ)});
            grid.mOrdinals.push_back(grid.mOrdinals.back() + uint64_t(countX) * uint64_t(countZ));
            if (countX == 1 && countZ == 1)
                break;
            countX = (countX + 1) / 2;
            countZ = (countZ + 1) / 2;
        }

        const TerrainLocalBounds &b = reference.Bounds();
        grid.mMeshed = {{detail::LocateAxis(reference, b.minX, b.maxX).cell,
                         detail::LocateAxis(reference, b.minZ, b.maxZ).cell},
                        {detail::LocateAxis(reference, b.maxX, b.maxX).cell + 1,
                         detail::LocateAxis(reference, b.maxZ, b.maxZ).cell + 1}};
        return grid;
    }

    bool TerrainRegionGrid::Contains(TerrainRegionCoord region) const
    {
        if (region.level >= Levels())
            return false;
        const TileCoord count = mCounts[region.level];
        return region.x >= 0 && region.z >= 0 && region.x < count.x && region.z < count.z;
    }

    uint64_t TerrainRegionGrid::Ordinal(TerrainRegionCoord region) const
    {
        return mOrdinals[region.level] + uint64_t(region.z) * uint64_t(mCounts[region.level].x) + uint64_t(region.x);
    }

    TerrainRegionCoord TerrainRegionGrid::AtOrdinal(uint64_t ordinal) const
    {
        const auto next = std::upper_bound(mOrdinals.begin(), mOrdinals.end(), ordinal);
        const uint32_t level = static_cast<uint32_t>(next - mOrdinals.begin() - 1);
        const uint64_t within = ordinal - mOrdinals[level];
        const uint64_t countX = uint64_t(mCounts[level].x);
        return {level, static_cast<int32_t>(within % countX), static_cast<int32_t>(within / countX)};
    }

    int64_t TerrainRegionGrid::FirstSampleX(TerrainRegionCoord region) const
    {
        return (int64_t(mFirstTile.x) + int64_t(region.x) * EdgeTiles(region.level)) * mReference.IntervalsPerTile();
    }

    int64_t TerrainRegionGrid::FirstSampleZ(TerrainRegionCoord region) const
    {
        return (int64_t(mFirstTile.z) + int64_t(region.z) * EdgeTiles(region.level)) * mReference.IntervalsPerTile();
    }

    TileRange TerrainRegionGrid::Tiles(TerrainRegionCoord region) const
    {
        const int64_t edge = EdgeTiles(region.level);
        const int64_t x0 = int64_t(mFirstTile.x) + int64_t(region.x) * edge;
        const int64_t z0 = int64_t(mFirstTile.z) + int64_t(region.z) * edge;
        return {{static_cast<int32_t>(x0), static_cast<int32_t>(z0)},
                {static_cast<int32_t>(x0 + edge - 1), static_cast<int32_t>(z0 + edge - 1)}};
    }

    TileRange TerrainRegionGrid::MapTiles(TerrainRegionCoord region) const
    {
        const TileRange all = Tiles(region);
        const TileRange map = mReference.Tiles();
        return {{std::max(all.min.x, map.min.x), std::max(all.min.z, map.min.z)},
                {std::min(all.max.x, map.max.x), std::min(all.max.z, map.max.z)}};
    }

    TerrainLocalBounds TerrainRegionGrid::Rect(TerrainRegionCoord region) const
    {
        const int64_t edge = EdgeTiles(region.level) * mReference.IntervalsPerTile();
        const int64_t x0 = FirstSampleX(region), z0 = FirstSampleZ(region);
        return {mReference.SamplePosition(x0), mReference.SamplePosition(z0), mReference.SamplePosition(x0 + edge),
                mReference.SamplePosition(z0 + edge)};
    }

    TerrainLocalBounds TerrainRegionGrid::ValidExtent(TerrainRegionCoord region) const
    {
        const TerrainLocalBounds rect = Rect(region);
        const TerrainLocalBounds &b = mReference.Bounds();
        return {std::max(rect.minX, b.minX), std::max(rect.minZ, b.minZ), std::min(rect.maxX, b.maxX),
                std::min(rect.maxZ, b.maxZ)};
    }

    SampleRange TerrainRegionGrid::Domain(TerrainRegionCoord region) const
    {
        const int64_t edge = EdgeTiles(region.level) * mReference.IntervalsPerTile();
        const int64_t x0 = FirstSampleX(region), z0 = FirstSampleZ(region);
        return {{std::max(x0, mMeshed.min.x), std::max(z0, mMeshed.min.z)},
                {std::min(x0 + edge, mMeshed.max.x), std::min(z0 + edge, mMeshed.max.z)}};
    }

    std::vector<int64_t> TerrainRegionGrid::Lines(int64_t first, uint32_t level, int64_t domainMin,
                                                  int64_t domainMax) const
    {
        const int64_t units = EdgeTiles(level) * mReference.IntervalsPerTile() / mUnit;
        const int64_t cells = std::min<int64_t>(mCells, units);
        std::vector<int64_t> lines{domainMin};
        // floor(i * units / cells) without overflow.
        const int64_t whole = units / cells, rest = units % cells;
        for (int64_t i = 1; i < cells; ++i)
        {
            const int64_t line = first + mUnit * (i * whole + (i * rest) / cells);
            if (line > domainMin && line < domainMax)
                lines.push_back(line);
        }
        lines.push_back(domainMax);
        return lines;
    }

    std::vector<int64_t> TerrainRegionGrid::LinesX(TerrainRegionCoord region) const
    {
        const SampleRange domain = Domain(region);
        return Lines(FirstSampleX(region), region.level, domain.min.x, domain.max.x);
    }

    std::vector<int64_t> TerrainRegionGrid::LinesZ(TerrainRegionCoord region) const
    {
        const SampleRange domain = Domain(region);
        return Lines(FirstSampleZ(region), region.level, domain.min.z, domain.max.z);
    }

    double TerrainRegionGrid::CellSpacing(uint32_t level) const
    {
        const int64_t units = EdgeTiles(level) * mReference.IntervalsPerTile() / mUnit;
        const int64_t cells = std::min<int64_t>(mCells, units);
        return mReference.SamplePosition(mUnit * ((units + cells - 1) / cells));
    }

    int32_t TerrainRegionGrid::SamplesPerSide(uint32_t level) const
    {
        const int64_t units = EdgeTiles(level) * mReference.IntervalsPerTile() / mUnit;
        return static_cast<int32_t>(std::min<int64_t>(mCells, units) + 1);
    }

    TerrainRegionCoord TerrainRegionGrid::Containing(uint32_t level, TileCoord tile) const
    {
        const int64_t edge = EdgeTiles(level);
        return {level, static_cast<int32_t>(FloorDiv(int64_t(tile.x) - mFirstTile.x, edge)),
                static_cast<int32_t>(FloorDiv(int64_t(tile.z) - mFirstTile.z, edge))};
    }

    std::optional<TerrainRegionCoord> TerrainRegionGrid::Parent(TerrainRegionCoord region) const
    {
        if (!Contains(region) || region.level + 1 >= Levels())
            return std::nullopt;
        return TerrainRegionCoord{region.level + 1, region.x / 2, region.z / 2};
    }

    std::vector<TerrainRegionCoord> TerrainRegionGrid::Children(TerrainRegionCoord region) const
    {
        std::vector<TerrainRegionCoord> children;
        if (!Contains(region) || region.level == 0)
            return children;
        for (int32_t dz = 0; dz < 2; ++dz)
            for (int32_t dx = 0; dx < 2; ++dx)
            {
                const TerrainRegionCoord child{region.level - 1, region.x * 2 + dx, region.z * 2 + dz};
                if (Contains(child))
                    children.push_back(child);
            }
        return children;
    }

    bool TerrainRegionGrid::SameGrid(const TerrainRegionGrid &other) const
    {
        const TerrainLayout &a = mReference, &b = other.mReference;
        return a.SampleSpacing() == b.SampleSpacing() && a.IntervalsPerTile() == b.IntervalsPerTile() &&
               a.Bounds() == b.Bounds() && mRegionTiles == other.mRegionTiles && mCells == other.mCells &&
               mUnit == other.mUnit;
    }
}
