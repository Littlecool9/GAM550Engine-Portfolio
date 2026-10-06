/**
 * @file TerrainGenerator.h
 * @author Jackey Cen
 * @brief Source heights of a recipe as a pure function of terrain-local sample positions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainNoise.h"
#include "TerrainRecipe.h"

#include <terrain/TerrainLayout.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <span>

namespace mir::terrainbake
{
    enum class SourceFillStatus
    {
        Ok,
        OutsideMap, // tile or samples beyond the map's tiles
        EmptyRange, // range min > max on an axis
        WrongSize,  // output span size differs from the sample count
        NonFinite,  // a height did not fit a finite float
        InvalidGrid, // not nested in the fixed reference grid
    };

    struct SourceFillResult
    {
        SourceFillStatus status = SourceFillStatus::Ok;
        GlobalSampleIndex sample; // first offending sample for NonFinite
        uint64_t referenceSamples = 0;
        uint64_t peakDoubleSamples = 0;

        bool Ok() const { return status == SourceFillStatus::Ok; }
    };

    // One tile's sample block, shared edges and padding included.
    size_t TileSampleCount(const TerrainLayout &layout);

    // Source heights: each depends only on the recipe and the sample's position.
    class TerrainGenerator
    {
    public:
        explicit TerrainGenerator(const TerrainRecipe &recipe);

        const TerrainLayout &Layout() const { return mLayout; }

    // Defined at any finite point, also off the map.
        double Height(double x, double z) const;
        float SampleHeight(GlobalSampleIndex sample) const;
        float SampleHeight(const TerrainLayout &layout, GlobalSampleIndex sample) const;
        SourceFillResult FillTile(const TerrainLayout &layout, TileCoord tile, std::span<float> out) const;
        SourceFillResult FillRange(const TerrainLayout &layout, const SampleRange &range, std::span<float> out) const;

        // TileHeights order; out is unspecified unless Ok.
        SourceFillResult FillTile(TileCoord tile, std::span<float> out) const;
        // Inclusive; must lie within the map's samples.
        SourceFillResult FillRange(const SampleRange &range, std::span<float> out) const;

    private:
        TerrainLayout mLayout;
        bool mFiltered = false;
        ReliefParameters mRelief;
        OctaveNoise mBroad;
        OctaveNoise mRidges;
        OctaveNoise mDetail;
        SampleRange mExtent;
    };
}
