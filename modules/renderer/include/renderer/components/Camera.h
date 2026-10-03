/**
 * @file Camera.h
 * @author Rahul Nair
 * @brief Marks an entity as the viewpoint RenderSystem projects from.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cmath>
#include <cstddef>

namespace mir
{

    struct Camera
    {
        float mFovYDegrees = 60.0f;
        float mNearZ = 0.1f;
        // 0 means no far plane. Depth is reverse-Z, so a large or missing far
        // plane costs no precision; it only decides what gets clipped.
        float mFarZ = 100.0f;

        // Anything not beyond the near plane counts as "none", so a bad value
        // cannot make a degenerate projection.
        bool HasFarPlane() const { return mFarZ > mNearZ && std::isfinite(mFarZ); }
    };

    MIR_ASSERT_COMPONENT(Camera);

    inline constexpr FieldDesc kCameraFields[] = {
        {"fovYDegrees", FieldKind::Float, offsetof(Camera, mFovYDegrees)},
        {"nearZ", FieldKind::Float, offsetof(Camera, mNearZ)},
        {"farZ", FieldKind::Float, offsetof(Camera, mFarZ)},
    };
}
