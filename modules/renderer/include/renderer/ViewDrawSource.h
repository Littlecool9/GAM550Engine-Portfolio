/**
 * @file ViewDrawSource.h
 * @author Jackey Cen
 * @brief Draws chosen per viewport, such as terrain detail, that RenderSystem asks for every frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "renderer/VulkanRenderer.h"
#include "renderer/components/Camera.h"

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <volk.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace mir
{
    // One viewport's camera for the frame.
    struct ViewCamera
    {
        // RenderSystem's ViewportId.
        size_t viewport = 0;
        WorldPose pose;
        Camera lens;
        // In pixels.
        VkExtent2D extent{};
        // Camera-relative, from MakeCameraRelativeViewProjection.
        glm::mat4 viewProj{1.0f};
    };

    // RenderSystem calls Prepare once a frame with every viewport that has a camera, before it asks for any
    // view's draws or for other volumes drawn at a view's detail, such as shadow casters.
    class IViewDrawSource
    {
    public:
        virtual ~IViewDrawSource() = default;

        virtual void Prepare(std::span<const ViewCamera> views) = 0;
        // view indexes the cameras given to Prepare.
        virtual void AppendViewDraws(size_t view, std::vector<WorldDrawItem> &out) = 0;
        // viewProj is relative to origin, as BuildViewDrawItems takes it.
        virtual void AppendVolumeDraws(size_t view, WorldPosition origin, const glm::mat4 &viewProj,
                                       std::vector<WorldDrawItem> &out) = 0;
    };

    // World resource; a source stays registered while it lives.
    struct ViewDrawSources
    {
        std::vector<IViewDrawSource *> sources;
    };
}
