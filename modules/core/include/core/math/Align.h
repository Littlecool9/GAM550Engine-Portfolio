/**
 * @file Align.h
 * @author Sumin Park
 * @brief Rounding a size up to an alignment.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <concepts>
#include <type_traits>

namespace mir
{
    // alignment must be a power of two, so the mask form is exact
    template <std::unsigned_integral T>
    constexpr T AlignUp(T n, std::type_identity_t<T> alignment)
    {
        return (n + alignment - 1) & ~(alignment - 1);
    }
}
