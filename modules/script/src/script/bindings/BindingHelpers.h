/**
 * @file BindingHelpers.h
 * @author Jackey Cen
 * @brief World positions to and from Lua {x, y, z} tables, read as double.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldReal.h>
#include <glm/vec3.hpp>
#include <sol/sol.hpp>

#include <optional>

namespace mir
{
    inline sol::table WorldVec3ToLua(sol::state_view lua, const WorldVec3 &value)
    {
        sol::table out = lua.create_table();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        return out;
    }

    inline std::optional<WorldVec3> WorldVec3FromLua(const sol::object &value)
    {
        if (!value.is<sol::table>())
            return std::nullopt;
        const sol::table t = value.as<sol::table>();
        return TryWorldVec3FromDouble(glm::dvec3(t.get_or("x", 0.0), t.get_or("y", 0.0), t.get_or("z", 0.0)));
    }
}
