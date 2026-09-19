/**
 * @file ComponentRegistration.cpp
 * @author Quy Dong Ta
 * @brief Registers every component the audio module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/ComponentRegistration.h>

#include <audio/components/AudioEmitter.h>

#include <core/ecs/ComponentRegistry.h>

namespace mir 
{
    void RegisterAudioComponents()
    {
        ComponentRegistry::Instance().Register<AudioEmitter>(kAudioEmitterFields);
    }
}
