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

#include <cfloat>
#include <cstddef>
#include <iterator>
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

    // Index == underlying LightType value. Shared by the Inspector's type combo
    // (kLightFields' "type" field) and the default name given to new light entities.
    inline constexpr std::string_view kLightTypeNames[] = { "Sun", "Point", "Spot", "Area" };

    inline const char *LightTypeName(LightType type)
    {
        const auto index = static_cast<std::size_t>(type);
        return index < std::size(kLightTypeNames) ? kLightTypeNames[index].data() : "?";
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
        { .mName = "type", .mKind = FieldKind::Int, .mOffset = offsetof(Light, mType),
          .mWidget = FieldWidget::Enum, .mEnumNames = kLightTypeNames },
        { .mName = "color", .mKind = FieldKind::Vec3, .mOffset = offsetof(Light, mColor),
          .mWidget = FieldWidget::Color },
        { .mName = "intensity", .mKind = FieldKind::Float, .mOffset = offsetof(Light, mIntensity),
          .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX }, // 0 is a legitimate "off"/degenerate value
        { .mName = "range", .mKind = FieldKind::Float, .mOffset = offsetof(Light, mRange),
          .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // 0 would mean "affects nothing, ever"
        { .mName = "spotAngleDegrees", .mKind = FieldKind::Float, .mOffset = offsetof(Light, mSpotAngleDegrees),
          .mClamped = true, .mMin = 1.0f, .mMax = 179.0f }, // full cone angle; >=180 or <=0 isn't a cone
        { .mName = "areaSize", .mKind = FieldKind::Float, .mOffset = offsetof(Light, mAreaSize),
          .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX }, // 0 is a legitimate "off"/degenerate value
    };
}
