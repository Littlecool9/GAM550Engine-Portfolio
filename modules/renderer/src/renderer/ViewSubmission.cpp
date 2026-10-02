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

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace mir
{
    namespace
    {
        // Translation plus vertex stays below 2^24 steps, which float holds exactly.
        constexpr double kExactGridSteps = 8388608.0;

        std::optional<glm::mat4> GridMatrixRelativeTo(const WorldPose &pose, WorldPosition camera, float grid)
        {
            const std::optional<WorldOffset> offset = TryWorldDifference(pose.position, camera);
            if (!offset)
                return std::nullopt;
            const glm::dvec3 steps = glm::dvec3(offset->Meters()) / double(grid);
            if (!(std::abs(steps.x) <= kExactGridSteps && std::abs(steps.y) <= kExactGridSteps &&
                  std::abs(steps.z) <= kExactGridSteps))
                return pose.TryMatrixRelativeTo(camera);

            glm::mat4 m(pose.linear);
            m[3] = glm::vec4(glm::vec3(glm::round(steps) * double(grid)), 1.0f);
            return m;
        }
    }

    glm::mat4 MakeCameraRelativeViewProjection(const WorldPose &camera, const Camera &lens, float aspectRatio)
    {
        return MakeViewProjection(glm::mat4(camera.linear), lens, aspectRatio);
    }

    size_t BuildViewDrawItems(std::span<const WorldDrawItem> items, WorldPosition viewportCamera,
                              const glm::mat4 &viewProj, std::vector<DrawItem> &out)
    {
        out.clear();
        out.reserve(items.size());

        size_t skipped = 0;
        for (const WorldDrawItem &item : items)
        {
            const std::optional<glm::mat4> model = item.translationGrid > 0.0f
                                                       ? GridMatrixRelativeTo(item.pose, viewportCamera,
                                                                              item.translationGrid)
                                                       : item.pose.TryMatrixRelativeTo(viewportCamera);
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
                // Clip w is the view depth: farther first, whatever the depth
                // mapping does with z.
                const glm::vec4 pa = viewProj * a.model * glm::vec4(0, 0, 0, 1);
                const glm::vec4 pb = viewProj * b.model * glm::vec4(0, 0, 0, 1);
                return pa.w > pb.w;
            });

        return skipped;
    }
}
