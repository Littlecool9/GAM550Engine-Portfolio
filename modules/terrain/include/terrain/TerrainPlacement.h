/**
 * @file TerrainPlacement.h
 * @author Jackey Cen
 * @brief Where one terrain instance sits in the fixed Scene world.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <core/math/WorldCoordinates.h>

namespace mir
{
    // Translation only: local XZ stays aligned with world XZ, and world height
    // is local height + translation.y. 
    struct TerrainPlacement
    {
        AssetId asset;
        WorldPosition translation;
    };
}
