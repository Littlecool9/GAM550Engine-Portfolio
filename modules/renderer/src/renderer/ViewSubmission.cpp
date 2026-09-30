/**
 * @file ViewSubmission.cpp
 * @author Jackey Cen
 * @brief Turns world-space draws into one viewport's camera-relative draw items.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/ViewSubmission.h"

#include "renderer/CameraMath.h"

#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace mir
{
    glm::mat4 MakeCameraRelativeViewProjection(const WorldPose &camera, const Camera &lens, float aspectRatio)
    {
        return MakeViewProjection(glm::mat4(camera.linear), lens.mFovYDegrees, lens.mNearZ, lens.mFarZ, aspectRatio);
    }

    size_t BuildViewDrawItems(std::span<const WorldDrawItem> items, WorldPosition viewportCamera,
                              const glm::mat4 &viewProj, std::vector<DrawItem> &out)
    {
        out.clear();
        out.reserve(items.size());

        size_t skipped = 0;
        for (const WorldDrawItem &item : items)
        {
            const std::optional<glm::mat4> model = item.pose.TryMatrixRelativeTo(viewportCamera);
            if (!model)
            {
                ++skipped;
                continue;
            }
            out.push_back(DrawItem{ item.mesh, *model, item.normalMatrix, item.tint, item.material,
                                    item.deformation, item.transparent });
        }

        std::stable_sort(out.begin(), out.end(),
            [&viewProj](const DrawItem &a, const DrawItem &b)
            {
                if (a.transparent != b.transparent)
                    return !a.transparent;
                if (!a.transparent)
                    return false;
                const glm::vec4 pa = viewProj * a.model * glm::vec4(0, 0, 0, 1);
                const glm::vec4 pb = viewProj * b.model * glm::vec4(0, 0, 0, 1);
                return pa.z / (std::abs(pa.w) > 1e-8f ? pa.w : 1e-8f) >
                       pb.z / (std::abs(pb.w) > 1e-8f ? pb.w : 1e-8f);
            });

        return skipped;
    }
}
