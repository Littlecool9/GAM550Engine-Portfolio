/**
 * @file Wwise.cpp
 * @author Dong Ta
 * @brief Thin queries against the live Wwise SDK - version string and
 *        whether the sound engine is initialized.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "audio/Wwise.h"

#include <AK/AkWwiseSDKVersion.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>

namespace mir::audio {

const char* WwiseSdkVersion()
{
    return AK_WWISESDK_VERSIONNAME;
}

bool IsSoundEngineInitialized()
{
    return AK::SoundEngine::IsInitialized();
}

} // namespace mir::audio