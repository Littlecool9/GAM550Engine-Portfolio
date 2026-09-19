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
    namespace
    {
        constexpr int kStopFadeMs = 10; // a hard stop mid-waveform clicks

        // Wwise stops nothing when a game object is unregistered, so every path
        // that ends a session or an entity has to release the voice itself.
        void StopVoice(AudioEmitter &emitter)
        {
            if (emitter.mPlayingId == 0)
                return;

            audio::StopPlaying(emitter.mPlayingId, kStopFadeMs);
            emitter.mPlayingId = 0;
        }
    }

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
                    // a looping voice outlives the session unless it is stopped here
                    StopVoice(emitter);
                    emitter.mStarted = false;
                }
                else if (simulating && emitter.mEventName[0] != '\0')
                {
                    if (emitter.mPlayingId != 0 && !audio::IsVoicePlaying(emitter.mPlayingId))
                        emitter.mPlayingId = 0;

                    // mLoop reposts whatever this emitter started, once it ends
                    const bool start = emitter.mPlayOnStart && !emitter.mStarted;
                    const bool repost = emitter.mLoop && emitter.mStarted && emitter.mPlayingId == 0;

                    if (start || repost)
                    {
                        emitter.mPlayingId = audio::PostEventOn(emitter.mEventName, emitter.mGameObject);
                        emitter.mStarted = true;
                    }
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
                    StopVoice(emitter);
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

        // stop first: unregistering leaves a loop playing, just beyond reach
        StopVoice(*emitter);
        audio::UnregisterEmitter(emitter->mGameObject);
        emitter->mGameObject = 0;
    }
}
