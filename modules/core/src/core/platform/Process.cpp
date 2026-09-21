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
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <cstddef>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        template <typename CharT> std::basic_string<CharT> Quote(std::basic_string_view<CharT> arg)
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
    } // namespace

    std::string QuoteCommandLineArgument(std::string_view arg) { return Quote(arg); }

    struct ChildProcess::Impl
    {
#if defined(_WIN32)
        HANDLE handle = nullptr;
#elif defined(__linux__)
        pid_t pid = -1;
#endif
        std::optional<int> exitCode;
    };

    ChildProcess::ChildProcess() : mImpl(std::make_unique<Impl>()) {}

    ChildProcess::~ChildProcess() { Terminate(); }

    ChildProcess::ChildProcess(ChildProcess &&) noexcept = default;
    ChildProcess &ChildProcess::operator=(ChildProcess &&) noexcept = default;

#if defined(_WIN32)
    bool ChildProcess::Start(const std::filesystem::path &program, std::span<const std::string> args,
                             const std::filesystem::path &workingDirectory)
    {
        if (mImpl->handle)
            return false;

        std::wstring commandLine = Quote(std::wstring_view(program.native()));
        for (const std::string &arg : args)
        {
            commandLine.push_back(L' ');
            commandLine += std::filesystem::path(QuoteCommandLineArgument(arg)).wstring();
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const wchar_t *workingDirectoryValue = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
        if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            workingDirectoryValue, &startup, &process))
        {
            MIR_LOG_ERROR("ChildProcess: could not start '{}' (error {})", program.string(), GetLastError());
            return false;
        }

        CloseHandle(process.hThread);
        mImpl->handle = process.hProcess;
        mImpl->exitCode.reset();
        return true;
    }

    std::optional<int> ChildProcess::Poll()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (!mImpl->handle)
            return std::nullopt;
        if (WaitForSingleObject(mImpl->handle, 0) == WAIT_TIMEOUT)
            return std::nullopt;

        DWORD code = 0;
        if (!GetExitCodeProcess(mImpl->handle, &code))
            code = static_cast<DWORD>(-1);
        CloseHandle(mImpl->handle);
        mImpl->handle = nullptr;
        mImpl->exitCode = static_cast<int>(code);
        return mImpl->exitCode;
    }

    bool ChildProcess::Running()
    {
        if (!mImpl->handle)
            return false;
        return !Poll().has_value();
    }

    std::optional<int> ChildProcess::Wait()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (!mImpl->handle)
            return std::nullopt;
        WaitForSingleObject(mImpl->handle, INFINITE);
        return Poll();
    }

    bool ChildProcess::Terminate()
    {
        if (!mImpl || !mImpl->handle)
            return false;
        const bool stopped = TerminateProcess(mImpl->handle, 1) != FALSE;
        WaitForSingleObject(mImpl->handle, INFINITE);
        Poll();
        return stopped;
    }
#elif defined(__linux__)
    bool ChildProcess::Start(const std::filesystem::path &program, std::span<const std::string> args,
                             const std::filesystem::path &workingDirectory)
    {
        if (mImpl->pid > 0)
            return false;

        const pid_t child = fork();
        if (child < 0)
        {
            MIR_LOG_ERROR("ChildProcess: could not fork for '{}'", program.string());
            return false;
        }
        if (child == 0)
        {
            if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0)
                _exit(127);
            std::vector<std::string> storage;
            storage.reserve(args.size() + 1);
            storage.push_back(program.string());
            storage.insert(storage.end(), args.begin(), args.end());
            std::vector<char *> argv;
            argv.reserve(storage.size() + 1);
            for (std::string &value : storage)
                argv.push_back(value.data());
            argv.push_back(nullptr);
            execv(program.c_str(), argv.data());
            _exit(127);
        }
        mImpl->pid = child;
        mImpl->exitCode.reset();
        return true;
    }

    std::optional<int> ChildProcess::Poll()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (mImpl->pid <= 0)
            return std::nullopt;
        int status = 0;
        const pid_t result = waitpid(mImpl->pid, &status, WNOHANG);
        if (result == 0)
            return std::nullopt;
        mImpl->pid = -1;
        mImpl->exitCode = result < 0 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return mImpl->exitCode;
    }

    bool ChildProcess::Running() { return mImpl->pid > 0 && !Poll().has_value(); }

    std::optional<int> ChildProcess::Wait()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (mImpl->pid <= 0)
            return std::nullopt;
        int status = 0;
        const pid_t result = waitpid(mImpl->pid, &status, 0);
        mImpl->pid = -1;
        mImpl->exitCode = result < 0 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return mImpl->exitCode;
    }

    bool ChildProcess::Terminate()
    {
        if (!mImpl || mImpl->pid <= 0)
            return false;
        const bool stopped = kill(mImpl->pid, SIGTERM) == 0;
        Wait();
        return stopped;
    }
#else
    bool ChildProcess::Start(const std::filesystem::path &, std::span<const std::string>, const std::filesystem::path &)
    {
        return false;
    }
    bool ChildProcess::Running() { return false; }
    std::optional<int> ChildProcess::Poll() { return std::nullopt; }
    std::optional<int> ChildProcess::Wait() { return std::nullopt; }
    bool ChildProcess::Terminate() { return false; }
#endif

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
        if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &process))
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
} // namespace mir
