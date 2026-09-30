/**
 * @file TileAddressing.h
 * @author Jackey Cen
 * @brief Terrain-local and world XZ positions to tile, within-tile offset and sample cell.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TileKey.h>

#include <glm/vec2.hpp>

#include <cstdint>

namespace mir
{
    enum class TileAddressStatus : uint8_t
    {
        Inside,
        OutsideMap,
        // The world-to-local subtraction overflowed; nothing was addressed.
        InvalidInput,
    };

    // Lower corner sample of the cell plus the position inside it, each axis in [0, 1].
    // The fraction is a rounded double: within an ulp below the next sample it
    // can already read 1. The origin index is exact and decides ownership.
    struct SampleCell
    {
        GlobalSampleIndex origin;
        glm::dvec2 fraction{0.0};
    };

    // Only status is meaningful unless it is Inside.
    struct TileAddress
    {
        TileAddressStatus status = TileAddressStatus::OutsideMap;
        TileCoord tile;
        // Meters from the tile's min corner, in [0, tile size]. 
        glm::dvec2 withinTile{0.0};
        SampleCell cell;

        bool IsInside() const { return status == TileAddressStatus::Inside; }
    };

    // How heights are interpolated inside a cell is the canonical surface's decision (#114).
    TileAddress AddressLocal(const TerrainLayout &layout, TerrainLocalPosition local);

    TileAddress AddressWorld(const TerrainLayout &layout, const TerrainPlacement &placement, WorldPosition world);
}
