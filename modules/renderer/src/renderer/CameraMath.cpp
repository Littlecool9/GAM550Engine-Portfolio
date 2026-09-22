/**
 * @file CameraMath.cpp
 * @author Rahul Nair
 * @brief Builds the view-projection matrix RenderSystem attaches to each
 *        RenderView, for the renderer's per-frame camera UBO.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

// GLM set to Vulkan clip space (0 ~ 1) vs OpenGL(-1 ~ 1)
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "renderer/CameraMath.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    glm::mat4 MakeViewProjection(const glm::mat4 &cameraWorld,
                                 float fovYDegrees, float nearZ, float farZ,
                                 float aspectRatio)
    {
        const glm::mat4 view = glm::inverse(cameraWorld);

        glm::mat4 proj = glm::perspective(glm::radians(fovYDegrees), aspectRatio, nearZ, farZ);

        // glm = Y up
        // vulkan = Y down
        // flip accordingly
        proj[1][1] *= -1.0f;

        return proj * view;
    }

    Ray MakeCameraRay(const glm::mat4 &cameraWorld, const glm::mat4 &viewProj, glm::vec2 ndc)
    {
        const glm::mat4 invViewProj = glm::inverse(viewProj);

        const glm::vec4 farH = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 farPoint = glm::vec3(farH) / farH.w;

        const glm::vec3 origin = glm::vec3(cameraWorld[3]);
        return Ray{ origin, glm::normalize(farPoint - origin) };
    }

    std::optional<float> RayIntersectsAABB(const Ray &ray, const glm::vec3 &boxMin, const glm::vec3 &boxMax)
    {
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();

        for (int axis = 0; axis < 3; ++axis)
        {
            const float origin = ray.origin[axis];
            const float dir = ray.direction[axis];
            const float minB = boxMin[axis];
            const float maxB = boxMax[axis];

            if (std::abs(dir) < 1e-8f)
            {
                if (origin < minB || origin > maxB)
                    return std::nullopt;
                continue;
            }

            float t1 = (minB - origin) / dir;
            float t2 = (maxB - origin) / dir;
            if (t1 > t2)
                std::swap(t1, t2);

            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return std::nullopt;
        }

        return tMin;
    }
}
