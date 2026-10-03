/**
 * @file main.cpp
 * @author Sumin Park
 * @brief Process entry point - parses launch args, resolves the game
 *        project, and drives App through one run (including a
 *        project-switch relaunch).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>
#include <app/GameProject.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <core/platform/Process.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    struct LaunchOptions
    {
        // empty: use engine.json's defaultGame
        std::string mGameDir;

        // 0: run until the window closes
        uint64_t mMaxFrames = 0;
        std::string mCapturePath;
        bool mNoEditor = false;
        bool mUIDemo = false;

        // test hook: games to restart into, in order
        std::vector<std::string> mProjectSwitches;
    };

    // [--game <dir>] [--frames <N>] [--switch-to <dir>]...; anything else fails
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
            else if (arg == "--capture" && i + 1 < argc)
            {
                options.mCapturePath = argv[++i];
            }
            else if (arg == "--no-editor")
            {
                options.mNoEditor = true;
            }
            else if (arg == "--ui-demo")
            {
                options.mUIDemo = true;
            }
            else if (arg == "--switch-to" && i + 1 < argc)
            {
                options.mProjectSwitches.emplace_back(argv[++i]);
            }
            else
            {
                MIR_LOG_ERROR("unrecognized argument '{}' (usage: [--game <dir>] [--frames <N>] [--capture <png>] [--no-editor] [--ui-demo] [--switch-to <dir>]...)", arg);
                return std::nullopt;
            }
        }

        // a switch happens halfway through --frames
        if (!options.mProjectSwitches.empty() && options.mMaxFrames == 0)
        {
            MIR_LOG_ERROR("--switch-to needs --frames");
            return std::nullopt;
        }

        if (!options.mCapturePath.empty() && options.mMaxFrames == 0)
        {
            MIR_LOG_ERROR("--capture needs --frames");
            return std::nullopt;
        }
        return options;
    }

    // Arguments for the next process; a bounded run passes on its remaining frames and switches
    std::vector<std::string> RelaunchArguments(const LaunchOptions &options, const mir::GameProject &next)
    {
        std::vector<std::string> args{"--game", next.mDir};
        if (options.mNoEditor)
            args.emplace_back("--no-editor");
        if (options.mUIDemo)
            args.emplace_back("--ui-demo");
        if (!options.mCapturePath.empty())
        {
            args.emplace_back("--capture");
            args.push_back(options.mCapturePath);
        }

        if (options.mMaxFrames != 0)
        {
            args.emplace_back("--frames");
            args.push_back(std::to_string(options.mMaxFrames - options.mMaxFrames / 2));

            for (std::size_t i = 1; i < options.mProjectSwitches.size(); ++i)
            {
                args.emplace_back("--switch-to");
                args.push_back(options.mProjectSwitches[i]);
            }
        }

        return args;
    }

    // Whether LogHistory holds an error
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

    // bad arguments or game directory exit before the splash
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
    desc.mCapturePath = options->mCapturePath;
    // a build without the editor is always the game; --no-editor is then a no-op
    desc.mEnableEditorLayout = mir::App::kHasEditor && !options->mNoEditor;
    desc.mUIDemo = options->mUIDemo;
    if (!options->mProjectSwitches.empty())
        desc.mSwitchProjectTo = options->mProjectSwitches.front();

    // also loads the game's scene
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

    // a bounded (test) run fails if anything logged an error
    const bool bounded = options->mMaxFrames != 0;
    const int exitCode = (bounded && AnyErrorLogged()) ? 1 : 0;

    // switching games restarts the process; a bounded run waits for the child
    if (const std::optional<mir::GameProject> &next = app.ProjectToOpen(); next.has_value())
    {
        const std::vector<std::string> args = RelaunchArguments(*options, *next);
        MIR_LOG_INFO("Restarting into '{}'", next->mDir);

        // the next process writes the same log file
        mir::ShutdownLog();

        const std::optional<int> childExit = mir::LaunchProcess(mir::ExecutablePath(), args, bounded);
        if (!childExit.has_value())
        {
            mir::InitLog();
            MIR_LOG_ERROR("could not restart into '{}'", next->mDir);
            mir::FlushLog();
            return 1;
        }

        return (bounded && exitCode == 0) ? *childExit : exitCode;
    }

    mir::FlushLog();
    return exitCode;
}
