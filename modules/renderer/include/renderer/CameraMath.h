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

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace mir
{
    glm::mat4 MakeViewProjection(const glm::mat4 &cameraWorld,
                                 float fovYDegrees, float nearZ, float farZ,
                                 float aspectRatio);

    struct Ray
    {
        glm::vec3 origin{ 0.0f };
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
    };

    Ray MakeCameraRay(const glm::mat4 &cameraWorld, const glm::mat4 &viewProj, glm::vec2 ndc);

    std::optional<float> RayIntersectsAABB(const Ray &ray, const glm::vec3 &boxMin, const glm::vec3 &boxMax);
}
