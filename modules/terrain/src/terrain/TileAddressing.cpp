/**
 * @file TileAddressing.cpp
 * @author Jackey Cen
 * @brief Terrain-local and world XZ positions to tile, within-tile offset and sample cell.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TileAddressing.h"

#include "AxisLocate.h"

namespace mir
{
    TileAddress AddressLocal(const TerrainLayout &layout, TerrainLocalPosition local)
    {
        const glm::dvec3 meters = local.Meters();
        const TerrainLocalBounds &bounds = layout.Bounds();

        // Checked in double before any integer conversion.
        if (!(meters.x >= bounds.minX && meters.x <= bounds.maxX && meters.z >= bounds.minZ &&
              meters.z <= bounds.maxZ))
            return {};

        const detail::AxisCell x = detail::LocateAxis(layout, meters.x, bounds.maxX);
        const detail::AxisCell z = detail::LocateAxis(layout, meters.z, bounds.maxZ);

        TileAddress address;
        address.status = TileAddressStatus::Inside;
        address.tile = {layout.TileOfCell(x.cell), layout.TileOfCell(z.cell)};
        address.withinTile = {meters.x - layout.SamplePosition(layout.TileFirstSample(address.tile.x)),
                              meters.z - layout.SamplePosition(layout.TileFirstSample(address.tile.z))};
        address.cell = {{x.cell, z.cell}, {x.fraction, z.fraction}};
        return address;
    }

    TileAddress AddressWorld(const TerrainLayout &layout, const TerrainPlacement &placement, WorldPosition world)
    {
        const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, placement.translation);
        if (!local)
        {
            TileAddress invalid;
            invalid.status = TileAddressStatus::InvalidInput;
            return invalid;
        }
        return AddressLocal(layout, *local);
    }
}
