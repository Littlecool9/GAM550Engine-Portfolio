/**
 * @file CameraMath.cpp
 * @author Rahul Nair
 * @brief Builds the view-projection matrix RenderSystem attaches to each
 *        RenderView, for the renderer's per-frame camera UBO.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

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

        // Vulkan clip depth is 0..1. Named outright: glm::perspective picks
        // its convention from a per-TU macro, and another TU without the
        // macro made the linker hand this one the -1..1 version.
        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(fovYDegrees), aspectRatio, nearZ, farZ);

        // glm = Y up
        // vulkan = Y down
        // flip accordingly
        proj[1][1] *= -1.0f;

        return proj * view;
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

    WorldRay MakeCameraRay(const WorldPose &camera, const Camera &lens, float aspectRatio, glm::vec2 ndc)
    {
        // Rotation only: the camera is the origin of this space, so the
        // unprojected point is already the direction.
        const glm::mat4 viewProj = MakeViewProjection(glm::mat4(camera.linear), lens.mFovYDegrees,
                                                      lens.mNearZ, lens.mFarZ, aspectRatio);

        const glm::vec4 farH = glm::inverse(viewProj) * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 farPoint = glm::vec3(farH) / farH.w;

        return WorldRay{ camera.position, glm::normalize(farPoint) };
    }

    std::optional<float> RayIntersectsBox(const WorldRay &ray, const WorldPose &box,
                                          const glm::vec3 &localMin, const glm::vec3 &localMax)
    {
        const std::optional<glm::mat4> boxFromRay = box.TryMatrixRelativeTo(ray.origin);
        if (!boxFromRay)
            return std::nullopt;

        // The ray starts at the origin of this space. An affine map keeps the
        // ray parameter, so the local t is still meters along the world ray.
        const glm::mat4 toLocal = glm::inverse(*boxFromRay);
        const glm::vec3 localOrigin = glm::vec3(toLocal[3]);
        const glm::vec3 localDirection = glm::mat3(toLocal) * ray.direction;

        return RayIntersectsAABB(Ray{ localOrigin, localDirection }, localMin, localMax);
    }
}
