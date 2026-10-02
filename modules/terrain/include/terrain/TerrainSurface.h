/**
 * @file TerrainSurface.h
 * @author Jackey Cen
 * @brief The canonical piecewise-linear surface over a release's final float32 samples.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileKey.h>

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>

namespace mir
{
    // Bump when the triangulation, ownership or evaluation changes; new sample values do not.
    inline constexpr uint32_t kTerrainSurfaceVersion = 1;

    // One tile's samples, not owned, in TileHeights order.
    struct TileSamplesView
    {
        TileCoord tile;
        std::span<const float> samples;
    };

    // Empty when the tile's samples are not available.
    using TileSampleLookup = std::function<std::span<const float>(TileCoord tile)>;

    // Cut along (i, k)-(i+1, k+1) as Jolt does; counter-clockwise seen from +Y.
    struct CellTriangulation
    {
        std::array<GlobalSampleIndex, 2> diagonal;
        // [0] holds dz >= dx (diagonal included, Jolt's tie rule), [1] dz < dx.
        std::array<std::array<GlobalSampleIndex, 3>, 2> triangles;
    };

    CellTriangulation TriangulateCell(GlobalSampleIndex cell);

    enum class SurfaceStatus : uint8_t
    {
        Ok,
        // Outside the closed bounds, even where a padding cell has samples.
        OutsideMap,
        // No lookup, a tile outside the layout, or a cell that does not hold the point.
        InvalidInput,
        MissingTile,
        // Wrong sample count, or a non-finite corner.
        InvalidSamples,
    };

    // height and normal are NaN unless Ok. tile and cell are meaningful unless OutsideMap.
    struct SurfaceSample
    {
        SurfaceStatus status = SurfaceStatus::OutsideMap;
        // Terrain-local meters.
        double height = std::numeric_limits<double>::quiet_NaN();
        // Of the triangle evaluated; it jumps across triangle edges.
        glm::dvec3 normal{std::numeric_limits<double>::quiet_NaN()};
        TileCoord tile;
        GlobalSampleIndex cell;
        uint8_t triangle = 0;

        bool IsOk() const { return status == SurfaceStatus::Ok; }
    };

    // Shared edges and corners belong to the larger index (AddressLocal). y is ignored.
    SurfaceSample EvaluateSurface(const TerrainLayout &layout, TerrainLocalPosition local,
                                  const TileSampleLookup &lookup);

    // The same surface in a cell the caller picks, e.g. the non-owning side of an edge.
    SurfaceSample EvaluateSurfaceInCell(const TerrainLayout &layout, const TileSamplesView &view,
                                        GlobalSampleIndex cell, TerrainLocalPosition local);

    // Half a float32 ulp of the largest |height|, at least 2^-24 m.
    double SurfaceTolerance(double maxAbsHeight);
}
