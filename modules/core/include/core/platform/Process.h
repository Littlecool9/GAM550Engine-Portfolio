/**
 * @file Process.h
 * @author Jackey Cen
 * @brief Starting another process.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mir
{
    class ChildProcess
    {
    public:
        ChildProcess();
        ~ChildProcess();

        ChildProcess(const ChildProcess &) = delete;
        ChildProcess &operator=(const ChildProcess &) = delete;
        ChildProcess(ChildProcess &&) noexcept;
        ChildProcess &operator=(ChildProcess &&) noexcept;

        bool Start(const std::filesystem::path &program, std::span<const std::string> args,
                   const std::filesystem::path &workingDirectory = {});
        bool Running();
        std::optional<int> Poll();
        std::optional<int> Wait();
        bool Terminate();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    // Quotes one argument for a Windows command line (CommandLineToArgvW rules)
    std::string QuoteCommandLineArgument(std::string_view arg);

    uint64_t CurrentProcessId();

    // Starts program with args in the current directory. With waitForExit returns
    // its exit code, otherwise 0 once started. Nullopt (logged) if it cannot start.
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit);
} // namespace mir
