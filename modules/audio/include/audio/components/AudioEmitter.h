/**
 * @file AudioEmitter.h
 * @author Quy Dong Ta
 * @brief
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <cstddef>
#include <cstdint>


namespace mir
{
    struct AudioEmitter
    {
        char mEventName[kFieldStringCapacity] = {};
        bool mPlayOnStart = false;

        bool mLoop = false;

        uint64_t mGameObject = 0;
        uint32_t mWorldVersion = 0; //last world transform version pushed
        uint32_t mPlayingId = 0;    //0 = nothing playing; drives the repost and the stop
        bool mStarted = false;
    };

    MIR_ASSERT_COMPONENT(AudioEmitter);

    inline constexpr FieldDesc kAudioEmitterFields[] = {
        {"eventName", FieldKind::String, offsetof(AudioEmitter, mEventName)},
        {"playOnStart", FieldKind::Bool, offsetof(AudioEmitter, mPlayOnStart)},
        {"loop", FieldKind::Bool, offsetof(AudioEmitter, mLoop)},
    };
    
}
