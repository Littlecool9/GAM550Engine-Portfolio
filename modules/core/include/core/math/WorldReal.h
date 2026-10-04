/**
 * @file WorldReal.h
 * @author Jackey Cen
 * @brief The scalar world positions are stored in, chosen by MIR_LARGE_WORLD.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "core/WorldConfig.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>

namespace mir
{
    inline constexpr bool kLargeWorld = MIR_LARGE_WORLD != 0;

    // Positions only. Rotation, scale, velocities and other bounded vectors stay float.
    using WorldReal = std::conditional_t<kLargeWorld, double, float>;
    using WorldVec3 = glm::vec<3, WorldReal>;
    using WorldMat3 = glm::mat<3, 3, WorldReal>;

    namespace detail
    {
        template <typename Real>
        bool FiniteCoordinates(const glm::vec<3, Real> &value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        template <typename Real>
        bool FiniteCoordinates(const glm::mat<3, 3, Real> &value)
        {
            return FiniteCoordinates(value[0]) && FiniteCoordinates(value[1]) && FiniteCoordinates(value[2]);
        }

        // nullopt for a non-finite value or one past To's range. 
        template <typename To, typename From>
        std::optional<glm::vec<3, To>> TryNarrowMeters(const glm::vec<3, From> &meters)
        {
            if constexpr (sizeof(To) < sizeof(From))
            {
                constexpr From kMax = static_cast<From>(std::numeric_limits<To>::max());
                // Written so NaN fails too.
                if (!(std::abs(meters.x) <= kMax && std::abs(meters.y) <= kMax && std::abs(meters.z) <= kMax))
                    return std::nullopt;
                return glm::vec<3, To>(meters);
            }
            else
            {
                if (!FiniteCoordinates(meters))
                    return std::nullopt;
                return glm::vec<3, To>(meters);
            }
        }
    }

    // For a position that arrives as double (JSON, Lua). A float build refuses
    // what float cannot hold instead of storing an infinity.
    [[nodiscard]] inline std::optional<WorldVec3> TryWorldVec3FromDouble(const glm::dvec3 &meters)
    {
        return detail::TryNarrowMeters<WorldReal>(meters);
    }
}
