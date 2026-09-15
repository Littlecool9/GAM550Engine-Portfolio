


#pragma once
namespace mir::audio
{
    //brings up wwise and loads banks
    //returns false if fails and engine proceeds without sound
    bool Init();

    void Update();
    
    //terminate/shutdown
    void Term();

    //plays an audio event given event name, audio listener/emiter will prob be handling this in futuer
    bool PostEvent(const char* eventName);
}
