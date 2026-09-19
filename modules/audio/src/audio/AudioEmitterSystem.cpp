/**
 * @file AudioEmitterSystem.cpp
 * @author Quy Dong Ta
 * @brief Game object lifetime and the WorldTransform -> Wwise position push.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/AudioEmitterSystem.h>

#include <audio/AudioSystem.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace mir
{
    void AudioEmitterSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<const WorldTransform, AudioEmitter>();
        context.world.AddDestroyHook(&AudioEmitterSystem::OnEntityDestroyed, this);
    }

    void AudioEmitterSystem::OnUpdate(SystemContext &context)
    {
        if (!audio::IsInitialized())
            return;

        const bool simulating = IsSimulating(context.world);

        const bool sessionOver = !IsPlaying(context.world);

        mQuery->ForEach([&](Entity, const WorldTransform &worldTransform, AudioEmitter &emitter)
            {
                if (emitter.mGameObject == 0)
                {
                    emitter.mGameObject = audio::RegisterEmitter("AudioEmitter");
                    if (emitter.mGameObject == 0)
                        return;

                    emitter.mWorldVersion = 0;
                }

                if (worldTransform.Version() != emitter.mWorldVersion)
                {
                    const glm::mat4 &matrix = worldTransform.Matrix();
                    audio::SetEmitterPosition(emitter.mGameObject,
                        glm::vec3(matrix[3]),
                        -glm::vec3(matrix[2]),  // -Z is forward
                        glm::vec3(matrix[1]));  // +Y is up

                    emitter.mWorldVersion = worldTransform.Version();
                }

                if (sessionOver)
                {
                    emitter.mStarted = false;
                }
                else if (simulating && emitter.mPlayOnStart && !emitter.mStarted
                    && emitter.mEventName[0] != '\0')
                {
                    audio::PostEventOn(emitter.mEventName, emitter.mGameObject);
                    emitter.mStarted = true;
                }
            });
    }

    void AudioEmitterSystem::OnStop(SystemContext &context)
    {
        if (mQuery != nullptr)
        {
            mQuery->ForEach(
                [](Entity, const WorldTransform &, AudioEmitter &emitter)
                {
                    audio::UnregisterEmitter(emitter.mGameObject);
                    emitter.mGameObject = 0;
                    emitter.mStarted = false;
                });
        }

        context.world.RemoveDestroyHook(&AudioEmitterSystem::OnEntityDestroyed, this);
    }

    void AudioEmitterSystem::OnEntityDestroyed(World &world, Entity entity, void *)
    {
        AudioEmitter *emitter = world.GetComponent<AudioEmitter>(entity);
        if (emitter == nullptr || emitter->mGameObject == 0)
            return;

        audio::UnregisterEmitter(emitter->mGameObject);
        emitter->mGameObject = 0;
    }
}
