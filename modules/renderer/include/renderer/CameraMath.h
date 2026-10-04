/**
 * @file CameraMath.h
 * @author Rahul Nair
 * @brief Builds the view-projection matrix RenderSystem attaches to each
 *        RenderView, for the renderer's per-frame camera UBO - plus the
 *        camera-ray and ray/AABB math viewport picking builds on top of it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "renderer/components/Camera.h"

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace mir
{
    // Reverse-Z for Vulkan: the near plane maps to depth 1 and the far plane to
    // 0, so the depth buffer clears to 0 and tests GREATER. A lens with no far
    // plane (Camera::HasFarPlane) gets depth = near / distance, which reaches 0
    // only at infinity.
    glm::mat4 MakeViewProjection(const glm::mat4 &cameraWorld, const Camera &lens, float aspectRatio);

    // A ray in a bounded float space, such as a mesh's local space.
    struct Ray
    {
        glm::vec3 origin{ 0.0f };
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
    };

    std::optional<float> RayIntersectsAABB(const Ray &ray, const glm::vec3 &boxMin, const glm::vec3 &boxMax);

    // A ray in the fixed Scene world: the origin keeps its world-precision position,
    // and only the bounded unit direction is float.
    struct WorldRay
    {
        WorldPosition origin;
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
    };

    // The ray through `ndc` of the viewport that `camera` and `lens` draw.
    WorldRay MakeCameraRay(const WorldPose &camera, const Camera &lens, float aspectRatio, glm::vec2 ndc);

    // A fixed-size, non-cascaded shadow frustum
    glm::mat4 MakeOrthographicViewProjection(const glm::vec3 &direction, float halfExtent, float nearZ);

    // A Spot/Point shadow frustum
    glm::mat4 MakeForwardZPerspectiveViewProjection(const glm::vec3 &forward, const glm::vec3 &up,
        float fovYDegrees, float nearZ, float farZ);

    // Distance in meters along the ray to a box given in `box`'s local space.
    // The box is placed relative to the ray origin in world precision before anything
    // narrows, so neither end is rounded to a far float position.
    std::optional<float> RayIntersectsBox(const WorldRay &ray, const WorldPose &box,
        const glm::vec3 &localMin, const glm::vec3 &localMax);
}
