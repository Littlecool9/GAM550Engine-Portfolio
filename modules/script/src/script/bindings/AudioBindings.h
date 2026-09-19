/**
 * @file AudioBindings.h
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:post_event/set_rtpc/set_switch/set_state.
 *
 * Must run after RegisterWorldBindings - extends its World usertype rather than
 * creating a new one. Every call looks the entity's AudioEmitter up again, so
 * nothing here caches an AkGameObject: AudioEmitterSystem only registers one on
 * its first update, and a handle cached at script start would stay 0 forever.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterAudioBindings(sol::state &lua);
}
