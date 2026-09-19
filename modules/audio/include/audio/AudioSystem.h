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
    bool PostEventOn(const char *eventName, uint64_t gameObject);
    bool IsInitialized();

}
