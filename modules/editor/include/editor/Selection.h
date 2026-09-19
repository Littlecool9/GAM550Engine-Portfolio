/**
 * @file Selection.h
 * @author Rahul Nair
 * @brief World resource holding the Hierarchy panel's current selection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

namespace mir
{
    struct Selection
    {
        Entity mSelected = kNullEntity;
    };
}
