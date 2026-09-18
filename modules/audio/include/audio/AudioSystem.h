/**
 * @file AudioSystem.h
 * @author Dong Ta
 * @brief Wwise sound engine lifecycle - init, per-frame update, event
 *        posting, and teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
namespace mir::audio
{

    bool Init();

    void Update();
    

    void Terminate();

    bool PostEvent(const char* eventName);

}
