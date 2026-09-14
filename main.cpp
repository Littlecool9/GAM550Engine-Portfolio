#include <app/App.h>
#include <app/GameProject.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    /// What the command line asked for. All optional: no --game loads
    /// engine.json's defaultGame, no --frames runs until the window closes.
    struct LaunchOptions
    {
        std::string mGameDir;
        uint64_t mMaxFrames = 0;

        // Test hook: games opened one after another during a bounded run
        std::vector<std::string> mProjectSwitches;
    };

    /// `[--game <dir>] [--frames <N>] [--switch-to <dir>]...`. Anything else is
    /// logged and fails, so a mistyped flag can't quietly fall through to the
    /// default game.
    std::optional<LaunchOptions> ParseLaunchOptions(int argc, char **argv)
    {
        LaunchOptions options;

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--game" && i + 1 < argc)
            {
                options.mGameDir = argv[++i];
            }
            else if (arg == "--frames" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                const auto [parsedEnd, ec] = std::from_chars(value.data(), value.data() + value.size(), options.mMaxFrames);
                if (ec != std::errc{} || parsedEnd != value.data() + value.size() || options.mMaxFrames == 0)
                {
                    MIR_LOG_ERROR("--frames expects a positive integer, got '{}'", value);
                    return std::nullopt;
                }
            }
            else if (arg == "--switch-to" && i + 1 < argc)
            {
                options.mProjectSwitches.emplace_back(argv[++i]);
            }
            else
            {
                MIR_LOG_ERROR("unrecognized argument '{}' (usage: [--game <dir>] [--frames <N>] [--switch-to <dir>]...)", arg);
                return std::nullopt;
            }
        }

        // Switches are spread across a bounded run; an unbounded one has no schedule for them
        if (!options.mProjectSwitches.empty() && options.mMaxFrames == 0)
        {
            MIR_LOG_ERROR("--switch-to needs --frames");
            return std::nullopt;
        }

        return options;
    }

    /// Whether the log history (the buffer behind the editor's Output panel)
    /// holds an error. It is a bounded ring, far larger than a smoke run logs.
    bool AnyErrorLogged()
    {
        for (const mir::LogEntry &entry : mir::LogHistory::Snapshot())
        {
            if (entry.level == mir::LogLevel::Error || entry.level == mir::LogLevel::Critical)
                return true;
        }
        return false;
    }
}

int main(int argc, char **argv)
{
    // Logging lives outside App so early construction failures are still visible.
    mir::InitLog();

    // Resolved before App exists: a bad argument or game directory exits
    // without ever showing the splash.
    const std::optional<LaunchOptions> options = ParseLaunchOptions(argc, argv);

    std::optional<std::string> requestedGame;
    if (options.has_value())
        requestedGame = options->mGameDir.empty() ? mir::ReadDefaultGame() : std::optional<std::string>(options->mGameDir);

    const std::optional<mir::GameProject> project =
        requestedGame.has_value() ? mir::ResolveGameProject(*requestedGame) : std::nullopt;
    if (!project.has_value())
    {
        mir::FlushLog();
        return 1;
    }

    mir::App app;

    mir::AppDesc desc{};
    desc.mProject = *project;
    desc.mMaxFrames = options->mMaxFrames;
    desc.mProjectSwitches = options->mProjectSwitches;

    // Opens the game's scene too
    if (!app.Initialize(desc))
    {
        mir::FlushLog();
        return -1;
    }

    // Only now is startup actually done - close explicitly rather than
    // leaning on WS_EX_TOPMOST z-order to hide the swap.
    app.Splash().SetProgress("Ready", 1.0f);
    app.Splash().Close();

    app.Run();
    app.Shutdown();

    // A bounded run is how the smoke tests drive this, and an error in the
    // log fails one as surely as a crash. Unbounded runs exit 0 as before.
    const int exitCode = (options->mMaxFrames != 0 && AnyErrorLogged()) ? 1 : 0;

    mir::FlushLog();
    return exitCode;
}
