/**
 * @file Wwise.h
 * @author Dong Ta
 * @brief Thin queries against the live Wwise SDK - version string and
 *        whether the sound engine is initialized.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir::audio {

    const char* WwiseSdkVersion();
    bool IsSoundEngineInitialized();

}