/**
 * @file AudioListener.h
 * @author Quy Dong Ta
 * @brief Marks the entity whose WorldTransform drives the Wwise listener.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <core/ecs/ComponentAsserts.h>
#include <cstdint>

namespace mir
{
    struct AudioListener
    {
        // last world transform version pushed; assigned at runtime, not authored
        uint32_t mWorldVersion = 0;
    };

    MIR_ASSERT_COMPONENT(AudioListener);
}
