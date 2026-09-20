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

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
}
