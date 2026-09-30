/**
 * @file Transform.h
 * @author Sumin Park
 * @brief Local translation/rotation/scale for an entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cmath>
#include <cstdint>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    class Transform
    {
    public:
        Transform() = default;

        explicit Transform(const glm::dvec3 &position,
                           const glm::quat &rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                           const glm::vec3 &scale = glm::vec3(1.0f))
            : mPosition(position), mRotation(rotation), mScale(scale)
        {
        }

        const glm::dvec3 &Position() const { return mPosition; }
        const glm::quat &Rotation() const { return mRotation; }
        const glm::vec3 &Scale() const { return mScale; }

        // Bumped on every mutation. Never 0, so 0 can mean "no such transform".
        uint32_t Version() const { return mVersion; }

        // Refuses a non-finite position: no write, no version bump.
        bool SetPosition(const glm::dvec3 &position)
        {
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                return false;
            mPosition = position;
            Touch();
            return true;
        }

        void SetRotation(const glm::quat &rotation)
        {
            mRotation = rotation;
            Touch();
        }

        void SetScale(const glm::vec3 &scale)
        {
            mScale = scale;
            Touch();
        }

        // Refuses a non-finite delta or result: no write, no version bump.
        bool Translate(const glm::dvec3 &delta) { return SetPosition(mPosition + delta); }

        // Applies `delta` on top of the current rotation, in local space.
        void Rotate(const glm::quat &delta)
        {
            mRotation = glm::normalize(mRotation * delta);
            Touch();
        }

        // rotate * scale, applied right to left. The translation stays in
        // Position(): no float matrix carries an absolute position.
        glm::mat3 LinearMatrix() const
        {
            glm::mat3 m = glm::mat3_cast(mRotation);
            m[0] *= mScale.x;
            m[1] *= mScale.y;
            m[2] *= mScale.z;
            return m;
        }

    private:
        // 0 is "never written"
        void Touch()
        {
            if (++mVersion == 0)
                mVersion = 1;
        }

        glm::dvec3 mPosition{0.0};
        glm::quat mRotation{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z - identity
        glm::vec3 mScale{1.0f};
        uint32_t mVersion = 1;
    };

    MIR_ASSERT_COMPONENT(Transform);

    // Transform as a script or inspector sees it
    // Accessor thunks
    inline constexpr FieldDesc kTransformFields[] = {
        // Narrowed to float until #105 widens the field for SceneIO and Lua.
        {"position", FieldKind::Vec3, 0,
         [](const void *component, void *out)
         { *static_cast<glm::vec3 *>(out) = glm::vec3(static_cast<const Transform *>(component)->Position()); },
         [](void *component, const void *in)
         { static_cast<Transform *>(component)->SetPosition(glm::dvec3(*static_cast<const glm::vec3 *>(in))); }},

        {"rotation", FieldKind::Quat, 0,
         [](const void *component, void *out)
         { *static_cast<glm::quat *>(out) = static_cast<const Transform *>(component)->Rotation(); },
         [](void *component, const void *in)
         { static_cast<Transform *>(component)->SetRotation(*static_cast<const glm::quat *>(in)); }},

        {"scale", FieldKind::Vec3, 0,
         [](const void *component, void *out)
         { *static_cast<glm::vec3 *>(out) = static_cast<const Transform *>(component)->Scale(); },
         [](void *component, const void *in)
         { static_cast<Transform *>(component)->SetScale(*static_cast<const glm::vec3 *>(in)); }},
    };
}
