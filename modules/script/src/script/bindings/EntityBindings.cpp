/**
 * @file EntityBindings.cpp
 * @author Rahul Nair
 * @brief Registers the Entity usertype for Lua scripts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "EntityBindings.h"

#include <core/ecs/Entity.h>

#include <sol/sol.hpp>

namespace mir
{
    void RegisterEntityBindings(sol::state &lua)
    {
        lua.new_usertype<Entity>("Entity",
                                  "is_null", &Entity::IsNull,
                                  sol::meta_function::equal_to, [](const Entity &a, const Entity &b)
                                  { return a == b; });
    }
}
