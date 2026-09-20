/**
 * @file CameraMath.h
 * @author Rahul Nair
 * @brief Builds the view-projection matrix RenderSystem attaches to each
 *        RenderView, for the renderer's per-frame camera UBO.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/mat4x4.hpp>

namespace mir
{
    glm::mat4 MakeViewProjection(const glm::mat4 &cameraWorld,
                                 float fovYDegrees, float nearZ, float farZ,
                                 float aspectRatio);
}
