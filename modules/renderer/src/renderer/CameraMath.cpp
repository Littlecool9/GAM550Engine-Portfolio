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
    glm::mat4 MakeViewProjection(const glm::mat4 &cameraWorld, const Camera &lens, float aspectRatio)
    {
        const glm::mat4 view = glm::inverse(cameraWorld);

        // Written out rather than taken from glm::perspective, which picks its
        // depth convention from a per-TU macro: a TU without the macro once
        // made the linker hand the renderer the -1..1 version.
        const float focal = 1.0f / std::tan(glm::radians(lens.mFovYDegrees) * 0.5f);
        const float n = lens.mNearZ;

        glm::mat4 proj{ 0.0f };
        proj[0][0] = focal / aspectRatio;
        proj[1][1] = -focal; // glm is Y up, Vulkan clip space is Y down
        proj[2][3] = -1.0f;  // clip w is the view depth

        // Float depth is densest near 0, and 1 / distance is steepest near the
        // camera: putting the far end at 0 spreads the precision evenly over
        // distance instead of spending it all next to the near plane.
        if (lens.HasFarPlane())
        {
            const float f = lens.mFarZ;
            proj[2][2] = n / (f - n);
            proj[3][2] = f * n / (f - n);
        }
        else
        {
            proj[3][2] = n; // the limit of the above as far goes to infinity
        }

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
        const glm::mat4 viewProj = MakeViewProjection(glm::mat4(camera.linear), lens, aspectRatio);

        // Depth 1 is the near plane: a finite point even with no far plane,
        // where depth 0 would be at infinity.
        const glm::vec4 nearH = glm::inverse(viewProj) * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 nearPoint = glm::vec3(nearH) / nearH.w;

        return WorldRay{ camera.position, glm::normalize(nearPoint) };
    }

    glm::mat4 MakeOrthographicViewProjection(const glm::vec3 &direction, float halfExtent, float nearZ)
    {
        const glm::vec3 fwd = glm::normalize(direction);
        const glm::vec3 upHint = std::abs(fwd.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(upHint, -fwd));
        const glm::vec3 up = glm::cross(-fwd, right);

        glm::mat4 cameraWorld{ 1.0f };
        cameraWorld[0] = glm::vec4(right, 0.0f);
        cameraWorld[1] = glm::vec4(up, 0.0f);
        cameraWorld[2] = glm::vec4(-fwd, 0.0f);
        cameraWorld[3] = glm::vec4(-fwd * (halfExtent + nearZ), 1.0f);

        const glm::mat4 view = glm::inverse(cameraWorld);
        const float farZ = nearZ + 2.0f * halfExtent;

        glm::mat4 proj{ 0.0f };
        proj[0][0] = 1.0f / halfExtent;
        proj[1][1] = -1.0f / halfExtent; // glm is Y up, Vulkan clip space is Y down
        proj[2][2] = -1.0f / (farZ - nearZ);
        proj[3][2] = -nearZ / (farZ - nearZ);
        proj[3][3] = 1.0f;

        return proj * view;
    }

    glm::mat4 MakeForwardZPerspectiveViewProjection(const glm::vec3 &forward, const glm::vec3 &up,
        float fovYDegrees, float nearZ, float farZ)
    {
        const glm::vec3 fwd = glm::normalize(forward);
        const glm::vec3 right = glm::normalize(glm::cross(up, -fwd));
        const glm::vec3 trueUp = glm::cross(-fwd, right);

        glm::mat4 cameraWorld{ 1.0f };
        cameraWorld[0] = glm::vec4(right, 0.0f);
        cameraWorld[1] = glm::vec4(trueUp, 0.0f);
        cameraWorld[2] = glm::vec4(-fwd, 0.0f);

        const glm::mat4 view = glm::inverse(cameraWorld);
        const float focal = 1.0f / std::tan(glm::radians(fovYDegrees) * 0.5f);

        glm::mat4 proj{ 0.0f };
        proj[0][0] = focal; // aspect is always 1: every shadow face is square
        proj[1][1] = -focal; // glm is Y up, Vulkan clip space is Y down
        proj[2][3] = -1.0f;  // clip w is the view depth
        // Forward Z (0=nearZ, 1=farZ)
        proj[2][2] = -farZ / (farZ - nearZ);
        proj[3][2] = -(farZ * nearZ) / (farZ - nearZ);

        return proj * view;
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
