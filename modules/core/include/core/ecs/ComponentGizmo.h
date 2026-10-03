/**
 * @file ComponentGizmo.h
 * @author Rahul Nair
 * @brief Seam for a component's owning module to draw viewport gizmos
 *        without depending on the editor's drawing backend (ImGui).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <cstdint>
#include <glm/vec3.hpp>

namespace mir
{
    // colorRGBA packs R@bits0-7, G@8-15, B@16-23, A@24-31 - same layout as ImGui's IM_COL32/ImU32.
    class GizmoCanvas
    {
    public:
        virtual ~GizmoCanvas() = default;

        virtual void Line(const glm::vec3 &a, const glm::vec3 &b, uint32_t colorRGBA, float thickness) const = 0;

        virtual void Circle(const glm::vec3 &center, const glm::vec3 &right, const glm::vec3 &up,
            float radius, uint32_t colorRGBA, int segments) const = 0;
    };
}
