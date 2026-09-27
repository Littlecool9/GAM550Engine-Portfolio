/**
 * @file Sky.h
 * @author Rahul Nair
 * @brief Marks an entity as the scene's sky.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"

namespace mir
{
    // Placeholder tag for now: no fields, nothing reads it yet. Nishita
    // atmosphere parameters and driving lightDirWS from this entity's
    // rotation are issue #97's scope.
    struct Sky
    {
    };

    MIR_ASSERT_COMPONENT(Sky);
}
