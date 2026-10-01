/**
 * @file ViewSubmission.h
 * @author Jackey Cen
 * @brief Turns world-space draws into one viewport's camera-relative draw items.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "renderer/VulkanRenderer.h"
#include "renderer/components/Camera.h"

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace mir
{
    // The camera's translation is left out: render space puts this camera at
    // the origin, so only its rotation (and any scale) reaches the view.
    glm::mat4 MakeCameraRelativeViewProjection(const WorldPose &camera, const Camera &lens, float aspectRatio);

    // The place a draw's world position converts from world precision to float.
    size_t BuildViewDrawItems(std::span<const WorldDrawItem> items, WorldPosition viewportCamera,
                              const glm::mat4 &viewProj, std::vector<DrawItem> &out);
}
