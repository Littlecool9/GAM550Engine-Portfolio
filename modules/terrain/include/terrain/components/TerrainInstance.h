/**
 * @file TerrainInstance.h
 * @author Jackey Cen
 * @brief A scene's reference to one terrain asset, placed at its entity's position.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>

namespace mir
{
    struct TerrainInstance
    {
        // Relative to assetsRoot; SceneIO truncates past 63 characters.
        char recipePath[kFieldStringCapacity] = {};
    };

    MIR_ASSERT_COMPONENT(TerrainInstance);

    inline constexpr FieldDesc kTerrainInstanceFields[] = {
        {"recipePath", FieldKind::String, offsetof(TerrainInstance, recipePath)},
    };
}
