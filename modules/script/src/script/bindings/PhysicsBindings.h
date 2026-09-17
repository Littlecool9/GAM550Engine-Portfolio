/**
 * @file PhysicsBindings.h
 * @author Jackey Cen
 * @brief  Must run after RegisterWorldBindings - extends its World usertype rather
 * than creating a new one. Every call looks the PhysicsWorld resource and the entity's 
 * RigidBody up again, so nothing here caches a body and the bindings work regardless of
 * registration-vs-emplacement order.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterPhysicsBindings(sol::state &lua);
}
