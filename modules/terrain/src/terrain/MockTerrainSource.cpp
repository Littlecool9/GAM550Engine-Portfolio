/**
 * @file MockTerrainSource.cpp
 * @author Jackey Cen
 * @brief Analytic stand-in for a terrain release, for checking addressing and identity end to end.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/MockTerrainSource.h"

#include <cmath>
#include <utility>

namespace mir
{
    MockTerrainSource::MockTerrainSource(const TerrainLayout &layout, const TerrainReleaseInfo &release,
                                         const TerrainPlacement &placement, MockHeightFunction height)
        : mLayout(layout), mRelease(release), mPlacement(placement), mHeight(std::move(height))
    {
    }

    std::optional<MockTerrainSource> MockTerrainSource::Create(const TerrainLayout &layout,
                                                               const TerrainReleaseInfo &release,
                                                               const TerrainPlacement &placement,
                                                               MockHeightFunction height)
    {
        if (release.asset.IsNull() || placement.asset != release.asset || !height)
            return std::nullopt;
        return MockTerrainSource(layout, release, placement, std::move(height));
    }

    std::optional<MockSample> MockTerrainSource::SampleHeightGlobal(GlobalSampleIndex sample) const
    {
        const TileRange tiles = mLayout.Tiles();
        const int64_t n = mLayout.IntervalsPerTile();
        if (sample.x < tiles.min.x * n || sample.x > (tiles.max.x + int64_t(1)) * n ||
            sample.z < tiles.min.z * n || sample.z > (tiles.max.z + int64_t(1)) * n)
            return std::nullopt;

        const double height = mHeight(mLayout.SamplePosition(sample.x), mLayout.SamplePosition(sample.z));
        if (!std::isfinite(height))
            return std::nullopt;
        return MockSample{height, mLayout.IsPaddingSample(sample)};
    }

    MockLookup MockTerrainSource::Lookup(WorldPosition world) const
    {
        MockLookup lookup;
        lookup.address = AddressWorld(mLayout, mPlacement, world);
        if (!lookup.address.IsInside())
            return lookup;
        lookup.key = TileKey{mRelease.asset, lookup.address.tile};
        lookup.release = mRelease;

        const GlobalSampleIndex origin = lookup.address.cell.origin;
        const GlobalSampleIndex corners[4]{
            origin, {origin.x + 1, origin.z}, {origin.x, origin.z + 1}, {origin.x + 1, origin.z + 1}};
        std::array<WorldPosition, 4> positions{};
        for (size_t i = 0; i < 4; ++i)
        {
            const std::optional<MockSample> sample = SampleHeightGlobal(corners[i]);
            if (!sample)
                return lookup;
            const std::optional<TerrainLocalPosition> local = TerrainLocalPosition::FromMeters(
                {mLayout.SamplePosition(corners[i].x), sample->localHeight, mLayout.SamplePosition(corners[i].z)});
            const std::optional<WorldPosition> placed =
                local ? TryTerrainLocalToWorld(*local, mPlacement.translation) : std::nullopt;
            if (!placed)
                return lookup;
            positions[i] = *placed;
        }

        lookup.cornerWorldPositions = positions;
        return lookup;
    }
}
