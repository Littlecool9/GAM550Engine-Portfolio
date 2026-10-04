/**
 * @file MockTerrainSource.h
 * @author Jackey Cen
 * @brief Analytic stand-in for a terrain release, for checking addressing and identity end to end.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TileAddressing.h>
#include <terrain/TileKey.h>

#include <array>
#include <functional>
#include <optional>

namespace mir
{
    // Local height (meters) at a terrain-local X, Z.
    using MockHeightFunction = std::function<double(double localX, double localZ)>;

    struct MockSample
    {
        double localHeight = 0.0;
        // Outside the bounds but part of an edge tile; see TerrainLayout::IsPaddingSample.
        bool padding = false;
    };

    struct MockLookup
    {
        TileAddress address;
        // Filled only when the address is Inside.
        std::optional<TileKey> key;
        std::optional<TerrainReleaseInfo> release;
        // Cell corners (i,k), (i+1,k), (i,k+1), (i+1,k+1) placed in the world
        // through TryTerrainLocalToWorld; absent if a height was non-finite or
        // placing it overflowed.
        std::optional<std::array<WorldPosition, 4>> cornerWorldPositions;
    };

    // Not a release reader or height query: no interpolation, no residency,
    // no files. It proves world position -> tile -> sample -> world position
    // and the identity chain, for tests and the P0 integration gate (#111).
    // The real path is #112 (packs), #114 (surface) and #117 (queries).
    // Temporary: once #117 lands and #111 moves to the real query, delete this
    // class and its tests. Engine, bake and query code must not depend on it.
    class MockTerrainSource
    {
    public:
        // Nullopt if the release has no asset or the placement names another asset.
        [[nodiscard]] static std::optional<MockTerrainSource> Create(
            const TerrainLayout &layout, const TerrainReleaseInfo &release, const TerrainPlacement &placement,
            MockHeightFunction height);

        const TerrainLayout &Layout() const { return mLayout; }
        const TerrainReleaseInfo &Release() const { return mRelease; }
        const TerrainPlacement &Placement() const { return mPlacement; }

        // Global indices, so a sample on a shared tile edge has one value.
        // Nullopt outside every map tile's samples or for a non-finite height.
        std::optional<MockSample> SampleHeightGlobal(GlobalSampleIndex sample) const;

        // OutsideMap and InvalidInput carry no heights, never a stand-in zero.
        MockLookup Lookup(WorldPosition world) const;

    private:
        MockTerrainSource(const TerrainLayout &layout, const TerrainReleaseInfo &release,
                          const TerrainPlacement &placement, MockHeightFunction height);

        TerrainLayout mLayout;
        TerrainReleaseInfo mRelease;
        TerrainPlacement mPlacement;
        MockHeightFunction mHeight;
    };
}
