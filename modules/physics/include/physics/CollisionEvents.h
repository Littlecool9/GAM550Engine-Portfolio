/**
 * @file CollisionEvents.h
 * @author Jackey Cen
 * @brief Collision begin/end events between two bodies, and the World resource scripts read them from.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace mir
{
    enum class CollisionPhase : uint8_t
    {
        Begin,
        End,
    };

    // One per body pair, not per contact point: Begin when the pair starts touching, End when
    // it stops or one body is destroyed. A pair that falls asleep in contact stays touching.
    struct CollisionEvent
    {
        CollisionPhase mPhase = CollisionPhase::Begin;
        BodyHandle mBodyA;
        BodyHandle mBodyB;
        // Read from the bodies when the event is made; kNullEntity for an untagged body.
        Entity mEntityA;
        Entity mEntityB;
        // End reuses the pair's last contact.
        WorldPosition mPoint;
        // Points from A toward B.
        glm::vec3 mNormal{ 0.0f };
    };

    // The events of the previous frame's physics substeps, published once per frame by
    // CollisionEventSystem. Empty outside Play.
    struct CollisionEvents
    {
        std::vector<CollisionEvent> mEvents;
    };
}
