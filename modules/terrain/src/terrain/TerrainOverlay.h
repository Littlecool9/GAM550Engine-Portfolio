/**
 * @file TerrainOverlay.h
 * @author Jackey Cen
 * @brief Internal: a mesh's exact deviation from one tile's final source over the triangle overlay.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLayout.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace mir::detail
{
    // Positions are tile-local. requireCover refuses a mesh whose bounds miss part of the valid extent.
    std::optional<double> OverlayDeviation(const TerrainLayout &layout, const TileHeights &source, TileCoord tile,
                                           std::span<const glm::dvec3> positions, std::span<const uint32_t> indices,
                                           bool requireCover);
}
