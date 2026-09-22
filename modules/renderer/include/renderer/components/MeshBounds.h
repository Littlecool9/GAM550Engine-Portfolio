/**
 * @file MeshBounds.h
 * @author Rahul Nair
 * @brief Local-space bounding box of an entity's mesh, for viewport picking.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"

#include <glm/vec3.hpp>

namespace mir
{
    struct MeshBounds
    {
        glm::vec3 mLocalMin{ 0.0f };
        glm::vec3 mLocalMax{ 0.0f };
    };

    MIR_ASSERT_COMPONENT(MeshBounds);
}
