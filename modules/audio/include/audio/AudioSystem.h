/**
 * @file AudioSystem.h
 * @author Dong Ta
 * @brief Wwise sound engine lifecycle - init, per-frame update, event
 *        posting, and teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <glm/vec3.hpp>
namespace mir::audio
{

    bool Init(const std::filesystem::path &bankDir);

    void Update();
    

    void Terminate();

    uint64_t RegisterEmitter(const char *debugName);
    
    void UnregisterEmitter(uint64_t gameObject);

    void SetEmitterPosition(uint64_t gameObject,
                            const glm::vec3 &position,
                            const glm::vec3 &front,
                            const glm::vec3 &up);

    void SetListenerPosition(const glm::vec3 &position,
                            const glm::vec3 &front,
                            const glm::vec3 &up);

    bool PostEvent(const char *eventName);

    uint32_t PostEventOn(const char *eventName, uint64_t gameObject);

    // A fadeMs of 10-20 keeps a stopped voice from clicking.
    void StopPlaying(uint32_t playingId, int fadeMs = 0);

    // True until Wwise reports audio has ended. Ends are collected on
    // the audio thread and applied in Update(), so this answers for last frame.
    bool IsVoicePlaying(uint32_t playingId);


    bool SetRTPCValue(const char *rtpcName, float value, uint64_t gameObject = 0);

    //switches are per game object, wwise has no global switch.
    bool SetSwitch(const char *switchGroup, const char *switchState, uint64_t gameObject);

    //states are global by design, no game object takes part.
    bool SetState(const char *stateGroup, const char *state);

    bool IsInitialized();

}
