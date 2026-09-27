/**
 * @file Name.h
 * @author Rahul Nair
 * @brief Optional display name for an entity - what the Hierarchy/Inspector
 *        show instead of "Entity N" when present.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cstring>
#include <string_view>

namespace mir
{
    struct Name
    {
        char mValue[kFieldStringCapacity] = {};
    };

    MIR_ASSERT_COMPONENT(Name);

    inline constexpr FieldDesc kNameFields[] = {
        { "name", FieldKind::String, offsetof(Name, mValue) },
    };

    // Truncates to fit, always null-terminated.
    inline Name MakeName(std::string_view value)
    {
        Name name{};
        const std::size_t count = value.size() < sizeof(name.mValue) - 1 ? value.size() : sizeof(name.mValue) - 1;
        std::memcpy(name.mValue, value.data(), count);
        return name;
    }
}
