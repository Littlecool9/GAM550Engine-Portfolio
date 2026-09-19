/**
 * @file AudioBindings.cpp
 * @author Quy Dong Ta
 * @brief The Lua audio surface, scoped to an entity's AudioEmitter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "AudioBindings.h"

#include <audio/AudioSystem.h>
#include <audio/components/AudioEmitter.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <sol/sol.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace mir
{
    namespace
    {
        // Same fade AudioEmitterSystem stops with - a hard stop mid-waveform clicks.
        constexpr int kStopFadeMs = 10;

        // Names arrive as sol::object rather than const char* so that a script
        // passing a number or nil is refused and logged, the way the physics
        // bindings refuse a malformed vector, instead of raising a Lua error.
        bool ToName(const sol::object &value, std::string_view binding, std::string &out)
        {
            if (!value.is<std::string>())
            {
                MIR_LOG_ERROR("script: world:{} needs a name string", binding);
                return false;
            }

            out = value.as<std::string>();
            return !out.empty();
        }

        // Any Lua number counts, tested by type rather than by is<float>(), which
        // is what the field bindings already do.
        bool ToNumber(const sol::object &value, std::string_view binding, float &out)
        {
            if (value.get_type() != sol::type::number)
            {
                MIR_LOG_ERROR("script: world:{} needs a number", binding);
                return false;
            }

            out = value.as<float>();
            return true;
        }

        // The one lookup every binding here starts from. Returns nullptr when the
        // entity has no AudioEmitter, and when it has one that AudioEmitterSystem
        // has not registered a game object for yet.
        AudioEmitter *ResolveEmitter(World &world, Entity entity)
        {
            AudioEmitter *emitter = world.GetComponent<AudioEmitter>(entity);
            if (emitter == nullptr || emitter->mGameObject == 0)
                return nullptr;

            return emitter;
        }

        uint32_t WorldPostEvent(World &world, Entity entity, const sol::object &eventName)
        {
            std::string name;
            if (!ToName(eventName, "post_event", name))
                return 0;

            AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return 0;

            const uint32_t playingId = audio::PostEventOn(name.c_str(), emitter->mGameObject);
            if (playingId == 0)
                return 0;

            // The voice has to be handed to AudioEmitterSystem, because that is the
            // only thing that stops one when play mode ends or the entity dies, and
            // it knows a single ID - mPlayingId. A script-posted loop left out of it
            // would keep playing past the session, out of everyone's reach.
            //
            // One emitter therefore carries one voice: this stops whatever the
            // emitter was playing, including its own mEventName.
            if (emitter->mPlayingId != 0)
                audio::StopPlaying(emitter->mPlayingId, kStopFadeMs);

            emitter->mPlayingId = playingId;
            emitter->mStarted = true;
            return playingId;
        }

        bool WorldStopEvent(World &world, Entity entity, sol::optional<sol::object> fadeMs)
        {
            AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr || emitter->mPlayingId == 0)
                return false;

            float fade = static_cast<float>(kStopFadeMs);
            if (fadeMs.has_value() && fadeMs->valid() && !ToNumber(*fadeMs, "stop_event", fade))
                return false;

            audio::StopPlaying(emitter->mPlayingId, static_cast<int>(fade));
            emitter->mPlayingId = 0;

            // mStarted stays true: it means "this emitter has already fired its
            // play-on-start", and a stop is not a reason to fire it again.
            return true;
        }

        bool WorldSetRtpc(World &world, Entity entity, const sol::object &rtpcName,
                          const sol::object &value)
        {
            std::string name;
            float number = 0.0f;
            if (!ToName(rtpcName, "set_rtpc", name) || !ToNumber(value, "set_rtpc", number))
                return false;

            const AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return false;

            return audio::SetRTPCValue(name.c_str(), number, emitter->mGameObject);
        }

        bool WorldSetSwitch(World &world, Entity entity, const sol::object &switchGroup,
                            const sol::object &switchState)
        {
            std::string group;
            std::string state;
            if (!ToName(switchGroup, "set_switch", group) || !ToName(switchState, "set_switch", state))
                return false;

            const AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return false;

            return audio::SetSwitch(group.c_str(), state.c_str(), emitter->mGameObject);
        }

        // No entity: a Wwise state is global, so scoping one to an emitter would be
        // a lie the bindings cannot keep.
        bool WorldSetState(World &, const sol::object &stateGroup, const sol::object &state)
        {
            std::string group;
            std::string value;
            if (!ToName(stateGroup, "set_state", group) || !ToName(state, "set_state", value))
                return false;

            return audio::SetState(group.c_str(), value.c_str());
        }
    }

    void RegisterAudioBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("post_event", &WorldPostEvent);
        worldType.set_function("stop_event", &WorldStopEvent);
        worldType.set_function("set_rtpc", &WorldSetRtpc);
        worldType.set_function("set_switch", &WorldSetSwitch);
        worldType.set_function("set_state", &WorldSetState);
    }
}
