/**
 * @file ComponentRegistration.h
 * @author Jackey Cen
 * @brief Registers every component the physics module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir
{
    // Mirrors RegisterRendererComponents / RegisterScriptComponents - core
    // cannot name RigidBody, so physics registers its own. Must run before any
    // scene is loaded, since SceneIO looks components up by name through the
    // registry - see App::Initialize.
    void RegisterPhysicsComponents();
}
