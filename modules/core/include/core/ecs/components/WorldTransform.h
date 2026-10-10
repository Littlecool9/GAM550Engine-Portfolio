/**
 * @file WorldTransform.h
 * @author Sumin Park
 * @brief Derived world pose plus the stamps that detect staleness.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"
#include "core/math/WorldCoordinates.h"

#include <cstdint>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <optional>

namespace mir
{
    namespace detail
    {
        struct TransformResolver;
    }

    // position is in world precision (WorldReal); rotation and scale stay float
    struct WorldPose
    {
        WorldPosition position;
        glm::mat3 linear{ 1.0f };

        // The only way to a float 4x4: the translation is position - origin,
        // subtracted in world precision. nullopt if that offset does not fit a float.
        [[nodiscard]] std::optional<glm::mat4> TryMatrixRelativeTo(WorldPosition origin) const
        {
            const std::optional<glm::vec3> translation = TryCameraRelativeFloat(position, origin);
            if (!translation)
                return std::nullopt;

            glm::mat4 m(linear);
            m[3] = glm::vec4(*translation, 1.0f);
            return m;
        }
    };

    // nullopt when the translation or the result is not a finite world position.
    [[nodiscard]] inline std::optional<WorldPose> TryComposePose(
        const WorldPose &parent, const glm::mat3 &localLinear, const WorldVec3 &localTranslation)
    {
        const std::optional<ParentLocalTranslation> translation =
            ParentLocalTranslation::FromMeters(localTranslation);
        const std::optional<WorldOffset> offset =
            translation ? TryParentLocalToWorldOffset(WorldMat3(parent.linear), *translation) : std::nullopt;
        const std::optional<WorldPosition> position =
            offset ? TryTranslateWorld(parent.position, *offset) : std::nullopt;
        if (!position)
            return std::nullopt;
        return WorldPose{ *position, parent.linear * localLinear };
    }

    // The translation a child of `parent` needs to sit at `world`. nullopt if
    // the parent's linear part cannot be inverted (a zero scale).
    [[nodiscard]] inline std::optional<ParentLocalTranslation> TryWorldToParentLocal(
        const WorldPose &parent, WorldPosition world)
    {
        const std::optional<WorldOffset> offset = TryWorldDifference(world, parent.position);
        return offset ? TryWorldOffsetToParentLocal(WorldMat3(parent.linear), *offset) : std::nullopt;
    }

    // Shear dropped, mirror goes on scale.x
    inline void DecomposeRotationScale(const glm::mat3 &linear, glm::quat &rotation, glm::vec3 &scale)
    {
        scale = { glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2]) };
        // mirror
        if (glm::determinant(linear) < 0.0f)
            scale.x = -scale.x;

        // a zero-length axis divides by 1 so the rotation stays finite.
        const glm::mat3 rotationMatrix(linear[0] / (scale.x != 0.0f ? scale.x : 1.0f),
            linear[1] / (scale.y != 0.0f ? scale.y : 1.0f),
            linear[2] / (scale.z != 0.0f ? scale.z : 1.0f));
        rotation = glm::quat_cast(rotationMatrix);
    }

    // Read it through ResolveWorld (core/ecs/TransformHierarchy.h)
    class WorldTransform
    {
    public:
        WorldTransform() = default;

        // Last computed pose. Current only after a resolve.
        const WorldPose &Pose() const { return mPose; }
        WorldPosition Position() const { return mPose.position; }
        const glm::mat3 &Linear() const { return mPose.linear; }

        // Bumped whenever the pose changes; this is what children stamp against.
        uint32_t Version() const { return mVersion; }

    private:
        friend struct detail::TransformResolver;

        WorldPose mPose;

        uint32_t mLocalVersion = 0;

        // Parent's WorldTransform::Version() at build time. 0 = was a root.
        uint32_t mParentVersion = 0;

        // Never 0, so a child can tell "root" from a real parent version.
        uint32_t mVersion = 1;

        // Forces the next resolve to rebuild regardless of the stamps.
        // Set on construction and on reparent
        bool mDirty = true;
    };

    MIR_ASSERT_COMPONENT(WorldTransform);

    // only getter, set is only to be done by Hierarchy System
    inline constexpr FieldDesc kWorldTransformFields[] = {
        { "position", FieldKind::WorldPosition, 0,
         [](const void *component, void *out)
         {
 *static_cast<WorldVec3 *>(out) = static_cast<const WorldTransform *>(component)->Position().Meters();
},
nullptr },
    };
}
