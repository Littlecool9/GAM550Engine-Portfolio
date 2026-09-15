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