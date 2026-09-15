#pragma once
namespace mir::audio
{

    bool Init();

    void Update();
    

    void Terminate();

    bool PostEvent(const char* eventName);

}
