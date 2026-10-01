/**
 * @file WorldCoordinates.h
 * @brief Fixed-Scene-world positions and explicit large-world conversions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */

#pragma once

#include "core/math/WorldReal.h"

#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace mir
{
    // Finite meters on some axes. Only Tag says which, so coordinates with
    // different Tags never convert into each other.
    template <typename Tag, typename Real = WorldReal>
    class Coordinate
    {
    public:
        using Vec = glm::vec<3, Real>;

        Coordinate() = default; // zero, e.g. the Scene origin for WorldPosition

        [[nodiscard]] static std::optional<Coordinate> FromMeters(const Vec &meters)
        {
            if (!detail::FiniteCoordinates(meters))
                return std::nullopt;
            return Coordinate(meters);
        }

        // glm would narrow a wider vector silently; use FromDoubleMeters.
        template <typename Other>
            requires(sizeof(Other) > sizeof(Real))
        static std::optional<Coordinate> FromMeters(const glm::vec<3, Other> &) = delete;

        // Range-checked: a float build refuses what float cannot hold.
        [[nodiscard]] static std::optional<Coordinate> FromDoubleMeters(const glm::dvec3 &meters)
        {
            const std::optional<Vec> narrowed = detail::TryNarrowMeters<Real>(meters);
            return narrowed ? std::optional<Coordinate>(Coordinate(*narrowed)) : std::nullopt;
        }

        [[nodiscard]] Vec Meters() const { return mMeters; }

    private:
        explicit Coordinate(const Vec &meters) : mMeters(meters) {}

        Vec mMeters{Real(0)};
    };

    // Declared only: a tag names a meaning and is never instantiated.
    namespace detail
    {
        struct WorldPositionTag;
        struct WorldOffsetTag;
        struct ParentLocalTag;
        struct TerrainLocalTag;
    }

    // Absolute Position
    using WorldPosition = Coordinate<detail::WorldPositionTag>;

    // A displacement expressed on the fixed world axes, not parent-local axes.
    using WorldOffset = Coordinate<detail::WorldOffsetTag>;

    // A child's translation in its parent's axes, before parent rotation/scale.
    using ParentLocalTranslation = Coordinate<detail::ParentLocalTag>;

    // A point in a terrain asset's unrotated, unscaled local coordinate grid.
    // Double in either build: tile identity is decided from it.
    using TerrainLocalPosition = Coordinate<detail::TerrainLocalTag, double>;

    // Fallible arithmetic rejects non-finite results, including overflow.
    [[nodiscard]] inline std::optional<WorldPosition> TryTranslateWorld(WorldPosition position, WorldOffset offset)
    {
        return WorldPosition::FromMeters(position.Meters() + offset.Meters());
    }

    [[nodiscard]] inline std::optional<WorldOffset> TryWorldDifference(WorldPosition point, WorldPosition origin)
    {
        return WorldOffset::FromMeters(point.Meters() - origin.Meters());
    }

    // parentLinear is the parent's rotation/scale in world precision.
    // A caller with float rotation/scale widens that bounded matrix explicitly.
    [[nodiscard]] inline std::optional<WorldOffset> TryParentLocalToWorldOffset(
        const WorldMat3 &parentLinear, ParentLocalTranslation translation)
    {
        if (!detail::FiniteCoordinates(parentLinear))
            return std::nullopt;
        return WorldOffset::FromMeters(parentLinear * translation.Meters());
    }

    // The inverse of TryParentLocalToWorldOffset. A parent that cannot be
    // inverted (a zero scale) yields a non-finite result, so it is nullopt.
    [[nodiscard]] inline std::optional<ParentLocalTranslation> TryWorldOffsetToParentLocal(
        const WorldMat3 &parentLinear, WorldOffset offset)
    {
        if (!detail::FiniteCoordinates(parentLinear))
            return std::nullopt;
        return ParentLocalTranslation::FromMeters(glm::inverse(parentLinear) * offset.Meters());
    }

    // Explicit narrowing for a bounded world-space displacement. This checks
    // representability. Rounding and underflow to zero are possible.
    [[nodiscard]] inline std::optional<glm::vec3> TryRendererLocalFloat(WorldOffset offset)
    {
        return detail::TryNarrowMeters<float>(offset.Meters());
    }

    // Use the camera belonging to the viewport being submitted. Never narrow
    // either absolute position before this world-precision subtraction.
    [[nodiscard]] inline std::optional<glm::vec3> TryCameraRelativeFloat(
        WorldPosition point, WorldPosition viewportCamera)
    {
        const auto offset = TryWorldDifference(point, viewportCamera);
        if (!offset)
            return std::nullopt;
        return TryRendererLocalFloat(*offset);
    }

    // Terrain placement is a translation only. It is not another Scene origin.
    // Both conversions add or subtract in double, then fit the result to its type.
    [[nodiscard]] inline std::optional<WorldPosition> TryTerrainLocalToWorld(
        TerrainLocalPosition local, WorldPosition instancePlacement)
    {
        return WorldPosition::FromDoubleMeters(glm::dvec3(instancePlacement.Meters()) + local.Meters());
    }

    [[nodiscard]] inline std::optional<TerrainLocalPosition> TryWorldToTerrainLocal(
        WorldPosition point, WorldPosition instancePlacement)
    {
        return TerrainLocalPosition::FromMeters(glm::dvec3(point.Meters()) - glm::dvec3(instancePlacement.Meters()));
    }
}
