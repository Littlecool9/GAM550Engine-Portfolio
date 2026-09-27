/**
 * @file Light.h
 * @author Rahul Nair
 * @brief Marks an entity as a light source.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"

namespace mir
{
    // Placeholder tag for now: no fields, nothing reads it yet. Sun/Point/
    // Spot/Area types and the shading data they carry are issue #98's scope.
    struct Light
    {
    };

    MIR_ASSERT_COMPONENT(Light);
}
