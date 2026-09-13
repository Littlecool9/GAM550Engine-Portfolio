#include <app/App.h>
#include <core/log/Log.h>

int main()
{
    // Logging lives outside App so early construction failures are still visible.
    mir::InitLog();

    mir::App app;

    mir::AppDesc desc{};
    // Single hardcoded project for now.
    desc.mProjectPath = "games/HelloWorld/game.json";
    desc.mProjectSettingsPath = "games/HelloWorld/project_settings.json";

    if (!app.Initialize(desc))
    {
        mir::FlushLog();
        return -1;
    }

    if (!app.LoadScene())
        MIR_LOG_ERROR("main: scene failed to load for project '{}'; starting with an empty World",
            desc.mProjectPath.string());


    app.Splash().SetProgress("Ready", 1.0f);
    app.Splash().Close();

    app.Run();
    app.Shutdown();

    mir::FlushLog();
    return 0;
}
