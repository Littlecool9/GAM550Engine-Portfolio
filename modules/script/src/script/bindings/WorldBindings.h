/**
 * @file WorldBindings.h
 * @author Rahul Nair
 * @brief Registers the World usertype: world:has/world:get
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    // Registers the World usertype: world:has/world:get
    void RegisterWorldBindings(sol::state &lua);
}
