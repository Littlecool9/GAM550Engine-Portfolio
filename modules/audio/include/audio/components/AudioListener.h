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
    // Exactly one active listener: the first entity the query reaches wins, any
    // others are ignored. Multiple listeners are out of scope.
    //
    // The Wwise listener object itself is registered in audio::Init and lives
    // until Terminate - this component supplies its position, not its lifetime.
    struct AudioListener
    {
        // last world transform version pushed; assigned at runtime, not authored
        uint32_t mWorldVersion = 0;
    };

    MIR_ASSERT_COMPONENT(AudioListener);
}
