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
#include <filesystem>
namespace mir::audio
{

    bool Init(const std::filesystem::path &bankDir);

    void Update();
    

    void Terminate();

    bool PostEvent(const char* eventName);

}
