/**
 * @file EntityBindings.h
 * @author Rahul Nair
 * @brief Registers the Entity usertype for Lua scripts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    // Registers the Entity usertype
    void RegisterEntityBindings(sol::state &lua);
}
