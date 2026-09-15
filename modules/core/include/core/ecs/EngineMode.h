/**
 * @file EngineMode.h
 * @author Sumin Park
 * @brief World resource for engine modes
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "World.h"

namespace mir
{
    enum class EngineMode
    {
        Edit,
        Play,
    };

    enum class PlayState
    {
        Running,
        Paused,
    };

    struct EngineModeState
    {
        EngineMode mMode = EngineMode::Edit;
        PlayState mPlayState = PlayState::Running;
    };

    inline EngineMode CurrentMode(const World &world) { return world.Resource<EngineModeState>().mMode; }

    inline bool IsEditing(const World &world) { return CurrentMode(world) == EngineMode::Edit; }

    inline bool IsPlaying(const World &world) { return CurrentMode(world) == EngineMode::Play; }

    inline bool IsPaused(const World &world)
    {
        const EngineModeState &state = world.Resource<EngineModeState>();
        return state.mMode == EngineMode::Play && state.mPlayState == PlayState::Paused;
    }

    inline bool IsSimulating(const World &world)
    {
        const EngineModeState &state = world.Resource<EngineModeState>();
        return state.mMode == EngineMode::Play && state.mPlayState == PlayState::Running;
    }

    inline void SetEngineMode(World &world, EngineMode mode) { world.Resource<EngineModeState>().mMode = mode; }

    inline void SetPlayState(World &world, PlayState state) { world.Resource<EngineModeState>().mPlayState = state; }
}
