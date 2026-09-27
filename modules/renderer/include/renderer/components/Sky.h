/**
 * @file Sky.h
 * @author Rahul Nair
 * @brief Marks an entity as the scene's sky and carries its Nishita atmosphere parameters
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cstddef>
#include <glm/vec3.hpp>

namespace mir
{
    enum class SkyMode : int32_t
    {
        Nishita = 0,
    };

    struct Sky
    {
        SkyMode mMode = SkyMode::Nishita;

        float mPlanetRadiusKm = 6371.0f;
        float mAtmosphereRadiusKm = 6471.0f;

        // Physical units are this * 1e-6 (m^-1).
        glm::vec3 mRayleighCoeff{ 5.5f, 13.0f, 22.4f };
        float mMieCoeff = 21.0f;

        float mRayleighScaleHeightKm = 8.0f;
        float mMieScaleHeightKm = 1.2f;

        // [0, 1); how forward-scattering Mie particles are.
        float mMieG = 0.758f;
        float mSunIntensity = 22.0f;
    };

    MIR_ASSERT_COMPONENT(Sky);

    inline constexpr FieldDesc kSkyFields[] = {
        { "mode", FieldKind::Int, offsetof(Sky, mMode) },
        { "planetRadiusKm", FieldKind::Float, offsetof(Sky, mPlanetRadiusKm) },
        { "atmosphereRadiusKm", FieldKind::Float, offsetof(Sky, mAtmosphereRadiusKm) },
        { "rayleighCoeff", FieldKind::Vec3, offsetof(Sky, mRayleighCoeff) },
        { "mieCoeff", FieldKind::Float, offsetof(Sky, mMieCoeff) },
        { "rayleighScaleHeightKm", FieldKind::Float, offsetof(Sky, mRayleighScaleHeightKm) },
        { "mieScaleHeightKm", FieldKind::Float, offsetof(Sky, mMieScaleHeightKm) },
        { "mieG", FieldKind::Float, offsetof(Sky, mMieG) },
        { "sunIntensity", FieldKind::Float, offsetof(Sky, mSunIntensity) },
    };
}
