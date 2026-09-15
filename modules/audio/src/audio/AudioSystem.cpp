#include "audio/AudioSystem.h"

#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>

#include "AkDefaultIOHookDeferred.h"

#ifndef AK_OPTIMIZED
#include <AK/Comm/AkCommunication.h>
#endif

#include "core/log/Log.h"
#include "core/fs/Paths.h"

namespace mir::audio {
    namespace {
    CAkDefaultIOHookDeferred g_lowLevelIO;
    bool g_ioInitialized = false;
    bool g_Initialized = false;
    
    //For testing purposes
    constexpr AkGameObjectID kGlobalObject = 1;
    constexpr AkGameObjectID kListener = 2;

    bool Check(AKRESULT result, const char* stage)
    {
        if (result != AK_Success)
        {
            MIR_LOG_ERROR("Wwise {} failed (AKRESULT {})", stage, static_cast<int>(result));
            return false;
        }
        return true;
    }

    }

    bool Init()
    {
        AkMemSettings memSettings;
        AK::MemoryMgr::GetDefaultSettings(memSettings);

        if (!Check(AK::MemoryMgr::Init(&memSettings), "MemoryMgr::Init"))
        {
            return false;
        }
        AkStreamMgrSettings stmSettings;
        AK::StreamMgr::GetDefaultSettings(stmSettings);
        
        if (AK::StreamMgr::Create(stmSettings) == nullptr)
        {
            MIR_LOG_ERROR("Wwise streammgr::create failed");
            Term();
            return false;
        }
        
        AkDeviceSettings deviceSettings;
        AK::StreamMgr::GetDefaultDeviceSettings(deviceSettings);
        if (!Check(g_lowLevelIO.Init(deviceSettings), "low level i/o init"))
        {
            Term();
            return false;
        }
        g_ioInitialized = true;

        AkInitSettings initSettings;
        AkPlatformInitSettings AkPlatformInitSettings;
        AK::SoundEngine::GetDefaultInitSettings(initSettings);

        AK::SoundEngine::GetDefaultPlatformInitSettings(AkPlatformInitSettings);
        if (!Check(AK::SoundEngine::Init(&initSettings, &AkPlatformInitSettings), "soundengine init"))
        {
            Term();
            return false;
        }
#ifndef AK_OPTIMIZED
        AkCommSettings commSettings;
        AK::Comm::GetDefaultInitSettings(commSettings);
        if (AK::Comm::Init(commSettings) != AK_Success)
        {
            MIR_LOG_WARN("wwise profiler connection unavailable");
        }
#endif
        const std::filesystem::path bankDir = ExecutableDir() / "wwise";
        g_lowLevelIO.SetBasePath(bankDir.c_str());
        AkBankID bankID;
        //init bank has bus layout other banks resolves against
        if (!Check(AK::SoundEngine::LoadBank("Init.bnk", bankID), "LoadBank(Init.bnk)")) 
        {
            Term();
            return false;
        }
        if (!Check(AK::SoundEngine::LoadBank("Test.bnk", bankID), "LoadBank(Test.bnk)"))
        {
            Term();
            return false;
        }
        AK::SoundEngine::RegisterGameObj(kListener, "Listener");
        AK::SoundEngine::SetDefaultListeners(&kListener, 1);
        AK::SoundEngine::RegisterGameObj(kGlobalObject, "Global");
        g_Initialized = true;
        MIR_LOG_INFO("wwise initialized");
        return true;
    }


    void Update()
    {
        if (g_Initialized)
        {
            AK::SoundEngine::RenderAudio();
        }
    }

    bool PostEvent(const char* eventName)
    {
        if (!g_Initialized)
            return false;

        if (AK::SoundEngine::PostEvent(eventName, kGlobalObject) == AK_INVALID_PLAYING_ID)
        {
            MIR_LOG_ERROR("wwise postevent ({}) failed", eventName);
            return false;
        }
        return true;
    }

    void Term()
    {
        if (AK::SoundEngine::IsInitialized()) 
        {
            AK::SoundEngine::UnregisterAllGameObj();
            AK::SoundEngine::ClearBanks();
#ifndef AK_OPTIMIZED
            AK::Comm::Term();
#endif
            AK::SoundEngine::Term();
        }

        if (g_ioInitialized) 
        {
            g_lowLevelIO.Term();
            g_ioInitialized = false;
        }

        if (AK::IAkStreamMgr::Get())
            AK::IAkStreamMgr::Get()->Destroy();

        if (AK::MemoryMgr::IsInitialized())
            AK::MemoryMgr::Term();

        g_Initialized = false;
    }

}
