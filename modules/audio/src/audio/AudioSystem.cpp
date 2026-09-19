/**
 * @file AudioSystem.cpp
 * @author Dong Ta
 * @brief Wwise sound engine lifecycle - init, per-frame update, event
 *        posting, and teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "audio/AudioSystem.h"

#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>

#include "AkDefaultIOHookDeferred.h"

#ifndef AK_OPTIMIZED
#include <AK/Comm/AkCommunication.h>
#endif

#include "core/log/Log.h"
#include <filesystem>
#include <glm/geometric.hpp>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace mir::audio {
    namespace {
        CAkDefaultIOHookDeferred g_lowLevelIO;
        bool g_ioInitialized = false;
        bool g_Initialized = false;
        

        constexpr AkGameObjectID kGlobalObject = 1;
        constexpr AkGameObjectID kListener = 2;


        AkGameObjectID g_nextEmitterId = 1000;


        bool g_warnedSetPositionFailed = false;
        bool Check(AKRESULT result, const char* stage)
        {
            if (result != AK_Success)
            {
                MIR_LOG_ERROR("Wwise {} failed (AKRESULT {})", stage, static_cast<int>(result));
                return false;
            }
            return true;
        }
        

        constexpr float kMinUpLengthSq = 1e-8f;

        AkSoundPosition MakeSoundPosition(const glm::vec3 &pos, const glm::vec3 &front, const glm::vec3 &up)
        {
            const bool d = glm::dot(front, front) <= 0.0f || glm::dot(up, up) <= 0.0f;
            const glm::vec3 f = d ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::normalize(front);
            const glm::vec3 rawUp = d ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(up);

            glm::vec3 ortho = rawUp - f * glm::dot(f, rawUp);
            if (glm::dot(ortho, ortho) <= kMinUpLengthSq)
            {
                // up ran parallel to front: any perpendicular will do
                const bool alignedWithY = f.y > 0.99f || f.y < -0.99f;
                const glm::vec3 reference = alignedWithY ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                         : glm::vec3(0.0f, 1.0f, 0.0f);
                ortho = reference - f * glm::dot(f, reference);
            }

            const glm::vec3 u = glm::normalize(ortho);

            AkSoundPosition out;
            out.Set(pos.x, pos.y, -pos.z, f.x, f.y, -f.z, u.x, u.y, -u.z);
            return out;
        }

        void ApplyPosition(AkGameObjectID gameObject, const AkSoundPosition &position, const char* what)
        {
            if (AK::SoundEngine::SetPosition(gameObject, position) == AK_Success)
                return;

            if (!g_warnedSetPositionFailed)
            {
                MIR_LOG_ERROR("wwise setposition rejected for {} (object {}); position updates are being dropped",
                    what, gameObject);
                g_warnedSetPositionFailed = true;
            }
        }

        // Playing IDs handed out by PostEventOn that have not ended yet.
        std::unordered_set<AkPlayingID> g_livePlayingIds;

        // AK_EndOfEvent fires on Wwise's audio processing thread, so the callback
        // only parks the ID here and Update() applies it on the main thread. The
        // lock covers a push or a swap and nothing else.
        std::mutex g_endedMutex;
        std::vector<AkPlayingID> g_endedPlayingIds;
        std::vector<AkPlayingID> g_endedScratch;

        void OnEventEnded(AkCallbackType type, AkEventCallbackInfo *eventInfo, void *, void *)
        {
            if (type != AK_EndOfEvent || eventInfo == nullptr)
                return;

            std::lock_guard<std::mutex> lock(g_endedMutex);
            g_endedPlayingIds.push_back(eventInfo->playingID);
        }
    }

    bool Init(const std::filesystem::path &bankDir)
    {

        std::error_code errorC;
        if (!std::filesystem::is_directory(bankDir, errorC))
        {
            MIR_LOG_INFO("audio: no SoundBanks at '{}', running silent", bankDir.string());
            return false;
        }
        

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
            Terminate();
            return false;
        }
        
        AkDeviceSettings deviceSettings;
        AK::StreamMgr::GetDefaultDeviceSettings(deviceSettings);
        if (!Check(g_lowLevelIO.Init(deviceSettings), "low level i/o init"))
        {
            Terminate();
            return false;
        }
        g_ioInitialized = true;

        AkInitSettings initSettings;
        AkPlatformInitSettings AkPlatformInitSettings;
        AK::SoundEngine::GetDefaultInitSettings(initSettings);

        AK::SoundEngine::GetDefaultPlatformInitSettings(AkPlatformInitSettings);
        if (!Check(AK::SoundEngine::Init(&initSettings, &AkPlatformInitSettings), "soundengine init"))
        {
            Terminate();
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

        g_lowLevelIO.SetBasePath(bankDir.c_str());
        AkBankID bankID;

        if (!Check(AK::SoundEngine::LoadBank("Init.bnk", bankID), "LoadBank(Init.bnk)")) 
        {
            Terminate();
            return false;
        }
        if (!Check(AK::SoundEngine::LoadBank("Test.bnk", bankID), "LoadBank(Test.bnk)"))
        {
            Terminate();
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
        if (!g_Initialized)
            return;

        AK::SoundEngine::RenderAudio();

        {
            std::lock_guard<std::mutex> lock(g_endedMutex);
            g_endedScratch.swap(g_endedPlayingIds);
        }

        for (const AkPlayingID playingId : g_endedScratch)
            g_livePlayingIds.erase(playingId);

        g_endedScratch.clear();
    }

    bool IsInitialized()
    {
        return g_Initialized;
    }

    uint64_t RegisterEmitter(const char* debugName)
    {
        if (!g_Initialized)
            return 0;
        
        const AkGameObjectID id = g_nextEmitterId++;
        if (!Check(AK::SoundEngine::RegisterGameObj(id, debugName), "RegisterGameObj"))
            return 0;
        return id;
    }

    void UnregisterEmitter(uint64_t gameObj)
    {
        //0 = unregistered, akinvalid unregisters every object including listener aka dont change
        if (!g_Initialized || gameObj == 0 || gameObj == AK_INVALID_GAME_OBJECT)
            return;

        AK::SoundEngine::UnregisterGameObj(static_cast<AkGameObjectID>(gameObj));
    }

    void SetEmitterPosition(uint64_t gameObject, const glm::vec3 &position, const glm::vec3 &front,
                            const glm::vec3 &up)
    {
        if (!g_Initialized || gameObject == 0)
            return;

        ApplyPosition(static_cast<AkGameObjectID>(gameObject), MakeSoundPosition(position, front, up), "emitter");
    }

    void SetListenerPosition(const glm::vec3 &position, const glm::vec3 &front, const glm::vec3 &up)
    {
        if (!g_Initialized)
            return;

        ApplyPosition(kListener, MakeSoundPosition(position, front, up), "listener");
    }

    bool PostEvent(const char* eventName)
    {
        return PostEventOn(eventName, kGlobalObject) != 0;
    }

    uint32_t PostEventOn(const char* eventName, uint64_t gameObject)
    {
        if (!g_Initialized)
            return 0;


        const AkPlayingID playingId = AK::SoundEngine::PostEvent(eventName,
            static_cast<AkGameObjectID>(gameObject), AK_EndOfEvent, &OnEventEnded);

        if (playingId == AK_INVALID_PLAYING_ID)
        {
            MIR_LOG_ERROR("wwise postevent ({}) on object {} failed", eventName, gameObject);
            return 0;
        }

        g_livePlayingIds.insert(playingId);
        return playingId;
    }

    //unregistering a game object does NOT stop its sounds,
    //so a loop has to be stopped through its playing ID.
    void StopPlaying(uint32_t playingId, int fadeMs)
    {
        if (!g_Initialized || playingId == AK_INVALID_PLAYING_ID)
            return;

        AK::SoundEngine::ExecuteActionOnPlayingID(AkActionOnEventType_Stop,
            static_cast<AkPlayingID>(playingId), static_cast<AkTimeMs>(fadeMs));

        //the stop still fires AK_EndOfEvent, dropping it now keeps a caller from
        //seeing a voice it has already stopped as playing for another frame
        g_livePlayingIds.erase(playingId);
    }

    bool IsVoicePlaying(uint32_t playingId)
    {
        return playingId != AK_INVALID_PLAYING_ID && g_livePlayingIds.count(playingId) != 0;
    }

    //al three take names rather than IDs, like PostEventOn, AK hashes the
    //string at the call, so nothing here needs the banks string table.

    bool SetRTPCValue(const char *rtpcName, float value, uint64_t gameObject)
    {
        if (!g_Initialized || rtpcName == nullptr)
            return false;

        //0 is the module's "no object" (RegisterEmitter hands out 1000 and up),
        //and AK spells that AK_INVALID_GAME_OBJECT, which is its global scope.
        const AkGameObjectID target = gameObject == 0
            ? AK_INVALID_GAME_OBJECT
            : static_cast<AkGameObjectID>(gameObject);

        return Check(AK::SoundEngine::SetRTPCValue(rtpcName, static_cast<AkRtpcValue>(value), target),
            "SetRTPCValue");
    }

    bool SetSwitch(const char *switchGroup, const char *switchState, uint64_t gameObject)
    {
        if (!g_Initialized || switchGroup == nullptr || switchState == nullptr || gameObject == 0)
            return false;

        return Check(AK::SoundEngine::SetSwitch(switchGroup, switchState,
            static_cast<AkGameObjectID>(gameObject)), "SetSwitch");
    }

    bool SetState(const char *stateGroup, const char *state)
    {
        if (!g_Initialized || stateGroup == nullptr || state == nullptr)
            return false;

        return Check(AK::SoundEngine::SetState(stateGroup, state), "SetState");
    }

    void Terminate()
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

        g_livePlayingIds.clear();
        {
            std::lock_guard<std::mutex> lock(g_endedMutex);
            g_endedPlayingIds.clear();
        }

        g_Initialized = false;
    }

}
