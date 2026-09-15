/**
 * @file Process.cpp
 * @author Jackey Cen
 * @brief Starting another process.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/platform/Process.h"

#include "core/log/Log.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <cstddef>
#include <vector>

namespace mir
{
    namespace
    {
        template <typename CharT>
        std::basic_string<CharT> Quote(std::basic_string_view<CharT> arg)
        {
            constexpr CharT kSpecial[] = {CharT(' '), CharT('\t'), CharT('\n'), CharT('\v'), CharT('"'), CharT(0)};

            if (!arg.empty() && arg.find_first_of(kSpecial) == std::basic_string_view<CharT>::npos)
                return std::basic_string<CharT>(arg);

            std::basic_string<CharT> quoted(1, CharT('"'));
            std::size_t backslashes = 0;

            for (const CharT c : arg)
            {
                if (c == CharT('\\'))
                {
                    ++backslashes;
                    continue;
                }

                if (c == CharT('"'))
                {
                    // double the backslashes, then escape the quote
                    quoted.append(backslashes * 2 + 1, CharT('\\'));
                }
                else
                {
                    quoted.append(backslashes, CharT('\\'));
                }

                quoted.push_back(c);
                backslashes = 0;
            }

            // so trailing backslashes don't escape the closing quote
            quoted.append(backslashes * 2, CharT('\\'));
            quoted.push_back(CharT('"'));
            return quoted;
        }
    }

    std::string QuoteCommandLineArgument(std::string_view arg)
    {
        return Quote(arg);
    }

#if defined(_WIN32)
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit)
    {
        std::wstring commandLine = Quote(std::wstring_view(program.native()));
        for (const std::string &arg : args)
        {
            // active code page, the same one the child's argv decodes with
            commandLine.push_back(L' ');
            commandLine += std::filesystem::path(QuoteCommandLineArgument(arg)).wstring();
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        // CreateProcessW needs a mutable command line
        if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &startup, &process))
        {
            MIR_LOG_ERROR("LaunchProcess: could not start '{}' (error {})", program.string(), GetLastError());
            return std::nullopt;
        }

        CloseHandle(process.hThread);

        int exitCode = 0;
        if (waitForExit)
        {
            WaitForSingleObject(process.hProcess, INFINITE);

            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            exitCode = static_cast<int>(code);
        }

        CloseHandle(process.hProcess);
        return exitCode;
    }
#elif defined(__linux__)
    // Untested: no preset builds Linux. A failed exec shows up as exit status 127.
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit)
    {
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(program.string());
        storage.insert(storage.end(), args.begin(), args.end());

        std::vector<char *> argv;
        argv.reserve(storage.size() + 1);
        for (std::string &value : storage)
            argv.push_back(value.data());
        argv.push_back(nullptr);

        pid_t pid = 0;
        if (posix_spawn(&pid, storage.front().c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        {
            MIR_LOG_ERROR("LaunchProcess: could not start '{}'", program.string());
            return std::nullopt;
        }

        if (!waitForExit)
            return 0;

        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
        {
            MIR_LOG_ERROR("LaunchProcess: lost track of '{}'", program.string());
            return std::nullopt;
        }

        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#else
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string>, bool)
    {
        MIR_LOG_ERROR("LaunchProcess: not supported on this platform, cannot start '{}'", program.string());
        return std::nullopt;
    }
#endif
}
