/**
 * @file AxisLocate.h
 * @author Jackey Cen
 * @brief Internal: one terrain-local coordinate to its sample-grid cell.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLayout.h>

#include <cstdint>

namespace mir::detail
{
    // SamplePosition(cell) <= x < SamplePosition(cell + 1) exactly, except on
    // the closed max edge. fraction = (x - SamplePosition(cell)) / spacing,
    // rounded, in [0, 1].
    struct AxisCell
    {
        int64_t cell = 0;
        double fraction = 0.0;
    };

    // Precondition: coordinate lies within the bounds on its axis; callers
    // check that first. On a max edge that is itself a sample the last cell is
    // used with fraction 1, so a cell never starts on the closed edge.
    AxisCell LocateAxis(const TerrainLayout &layout, double coordinate, double axisMax);
}
