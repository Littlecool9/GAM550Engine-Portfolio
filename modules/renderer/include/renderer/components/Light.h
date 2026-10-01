/**
 * @file Light.h
 * @author Rahul Nair
 * @brief Marks an entity as a light source - sun/point/spot/area.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cstddef>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    enum class LightType : int32_t
    {
        // Directional: Transform rotation only, no position/range/cone
        Sun = 0,
        // Point/Spot/Area go into VulkanRenderer's per-frame light list
        Point = 1,
        Spot = 2,
        Area = 3,
    };

    inline const glm::vec3 kSunReferenceLocalDirection = glm::normalize(glm::vec3(0.4f, 0.7f, 0.5f));

    // Shared by the Inspector's type combo and the default name given to new light entities.
    inline const char *LightTypeName(LightType type)
    {
        switch (type)
        {
        case LightType::Sun:
            return "Sun";
        case LightType::Point:
            return "Point";
        case LightType::Spot:
            return "Spot";
        case LightType::Area:
            return "Area";
        }
        return "?";
    }

    struct Light
    {
        LightType mType = LightType::Point;
        glm::vec3 mColor{ 1.0f, 1.0f, 1.0f };
        float mIntensity = 1.0f;
        float mRange = 2.5f;
        float mSpotAngleDegrees = 45.0f;
        float mAreaSize = 0.5f;
    };

    MIR_ASSERT_COMPONENT(Light);

    inline constexpr FieldDesc kLightFields[] = {
        { "type", FieldKind::Int, offsetof(Light, mType) },
        { "color", FieldKind::Vec3, offsetof(Light, mColor) },
        { "intensity", FieldKind::Float, offsetof(Light, mIntensity) },
        { "range", FieldKind::Float, offsetof(Light, mRange) },
        { "spotAngleDegrees", FieldKind::Float, offsetof(Light, mSpotAngleDegrees) },
        { "areaSize", FieldKind::Float, offsetof(Light, mAreaSize) },
    };
}
