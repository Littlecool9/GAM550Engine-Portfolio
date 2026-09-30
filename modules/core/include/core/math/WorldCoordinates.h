/**
 * @file WorldCoordinates.h
 * @brief Fixed-Scene-world positions and explicit large-world conversions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */

#pragma once

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace mir
{
    namespace detail
    {
        inline bool FiniteCoordinates(const glm::dvec3 &value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        inline bool FiniteCoordinates(const glm::dmat3 &value)
        {
            return FiniteCoordinates(value[0]) &&
                   FiniteCoordinates(value[1]) &&
                   FiniteCoordinates(value[2]);
        }
    }

    // Absolute Position
    class WorldPosition
    {
    public:
        WorldPosition() = default; // Scene origin

        [[nodiscard]] static std::optional<WorldPosition> FromMeters(const glm::dvec3 &meters)
        {
            if (!detail::FiniteCoordinates(meters))
                return std::nullopt;
            return WorldPosition(meters);
        }

        [[nodiscard]] glm::dvec3 Meters() const { return mMeters; }

    private:
        explicit WorldPosition(const glm::dvec3 &meters) : mMeters(meters) {}

        glm::dvec3 mMeters{0.0};
    };

    // A displacement expressed on the fixed world axes, not parent-local axes.
    class WorldOffset
    {
    public:
        WorldOffset() = default;

        [[nodiscard]] static std::optional<WorldOffset> FromMeters(const glm::dvec3 &meters)
        {
            if (!detail::FiniteCoordinates(meters))
                return std::nullopt;
            return WorldOffset(meters);
        }

        [[nodiscard]] glm::dvec3 Meters() const { return mMeters; }

    private:
        explicit WorldOffset(const glm::dvec3 &meters) : mMeters(meters) {}

        glm::dvec3 mMeters{0.0};
    };

    // A child's translation in its parent's axes, before parent rotation/scale.
    class ParentLocalTranslation
    {
    public:
        ParentLocalTranslation() = default;

        [[nodiscard]] static std::optional<ParentLocalTranslation> FromMeters(const glm::dvec3 &meters)
        {
            if (!detail::FiniteCoordinates(meters))
                return std::nullopt;
            return ParentLocalTranslation(meters);
        }

        [[nodiscard]] glm::dvec3 Meters() const { return mMeters; }

    private:
        explicit ParentLocalTranslation(const glm::dvec3 &meters) : mMeters(meters) {}

        glm::dvec3 mMeters{0.0};
    };

    // A point in a terrain asset's unrotated, unscaled local coordinate grid.
    class TerrainLocalPosition
    {
    public:
        TerrainLocalPosition() = default;

        [[nodiscard]] static std::optional<TerrainLocalPosition> FromMeters(const glm::dvec3 &meters)
        {
            if (!detail::FiniteCoordinates(meters))
                return std::nullopt;
            return TerrainLocalPosition(meters);
        }

        [[nodiscard]] glm::dvec3 Meters() const { return mMeters; }

    private:
        explicit TerrainLocalPosition(const glm::dvec3 &meters) : mMeters(meters) {}

        glm::dvec3 mMeters{0.0};
    };

    // Fallible arithmetic rejects non-finite results, including double overflow.
    [[nodiscard]] inline std::optional<WorldPosition> TryTranslateWorld(WorldPosition position, WorldOffset offset)
    {
        return WorldPosition::FromMeters(position.Meters() + offset.Meters());
    }

    [[nodiscard]] inline std::optional<WorldOffset> TryWorldDifference(WorldPosition point, WorldPosition origin)
    {
        return WorldOffset::FromMeters(point.Meters() - origin.Meters());
    }

    // parentLinear is the parent's rotation/scale as a double linear matrix.
    // A caller with float rotation/scale widens that bounded matrix explicitly.
    [[nodiscard]] inline std::optional<WorldOffset> TryParentLocalToWorldOffset(
        const glm::dmat3 &parentLinear, ParentLocalTranslation translation)
    {
        if (!detail::FiniteCoordinates(parentLinear))
            return std::nullopt;
        return WorldOffset::FromMeters(parentLinear * translation.Meters());
    }

    // Explicit narrowing for a bounded world-space displacement. This checks
    // representability. Rounding and underflow to zero are possible.
    [[nodiscard]] inline std::optional<glm::vec3> TryRendererLocalFloat(WorldOffset offset)
    {
        const glm::dvec3 meters = offset.Meters();
        const double maxFloat = static_cast<double>(std::numeric_limits<float>::max());
        if (!detail::FiniteCoordinates(meters) ||
            std::abs(meters.x) > maxFloat ||
            std::abs(meters.y) > maxFloat ||
            std::abs(meters.z) > maxFloat)
            return std::nullopt;

        const glm::vec3 result(meters);
        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
            return std::nullopt;
        return result;
    }

    // Use the camera belonging to the viewport being submitted. Never narrow
    // either absolute position before this double-precision subtraction.
    [[nodiscard]] inline std::optional<glm::vec3> TryCameraRelativeFloat(
        WorldPosition point, WorldPosition viewportCamera)
    {
        const auto offset = TryWorldDifference(point, viewportCamera);
        if (!offset)
            return std::nullopt;
        return TryRendererLocalFloat(*offset);
    }

    // Terrain placement is a translation only. It is not another Scene origin.
    [[nodiscard]] inline std::optional<WorldPosition> TryTerrainLocalToWorld(
        TerrainLocalPosition local, WorldPosition instancePlacement)
    {
        const auto offset = WorldOffset::FromMeters(local.Meters());
        return offset ? TryTranslateWorld(instancePlacement, *offset) : std::nullopt;
    }

    [[nodiscard]] inline std::optional<TerrainLocalPosition> TryWorldToTerrainLocal(
        WorldPosition point, WorldPosition instancePlacement)
    {
        const auto offset = TryWorldDifference(point, instancePlacement);
        return offset ? TerrainLocalPosition::FromMeters(offset->Meters()) : std::nullopt;
    }
}
