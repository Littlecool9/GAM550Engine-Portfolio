/**
 * @file AudioListenerSystem.cpp
 * @author Quy Dong Ta
 * @brief The WorldTransform -> Wwise listener position push.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/AudioListenerSystem.h>

#include <audio/AudioSystem.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    void AudioListenerSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<const WorldTransform, AudioListener>();
    }

    void AudioListenerSystem::OnUpdate(SystemContext &)
    {
        if (!audio::IsInitialized())
            return;

        bool pushed = false;

        mQuery->ForEach([&](Entity, const WorldTransform &worldTransform, AudioListener &listener)
            {
                if (pushed)
                {
                    if (!mWarnedMultipleListeners)
                    {
                        MIR_LOG_WARN("more than one AudioListener in the scene; "
                            "only the first is driving Wwise");
                        mWarnedMultipleListeners = true;
                    }
                    return;
                }

                pushed = true;

                if (worldTransform.Version() == listener.mWorldVersion)
                    return;

                const glm::mat4 &matrix = worldTransform.Matrix();
                audio::SetListenerPosition(glm::vec3(matrix[3]),
                    -glm::vec3(matrix[2]),  // -Z is forward
                    glm::vec3(matrix[1]));  // +Y is up

                listener.mWorldVersion = worldTransform.Version();
            });
    }
}
