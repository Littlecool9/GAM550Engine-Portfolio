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

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <optional>

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

                // Relative to the Scene origin, so far positions still round in
                // float; no P0 issue owns audio yet (see terrain ADR 0002).
                const std::optional<glm::vec3> position =
                    TryCameraRelativeFloat(worldTransform.Position(), WorldPosition{});
                if (!position)
                    return;

                const glm::mat3 &linear = worldTransform.Linear();
                audio::SetListenerPosition(*position,
                    -linear[2],  // -Z is forward
                    linear[1]);  // +Y is up

                listener.mWorldVersion = worldTransform.Version();
            });
    }
}
