/**
 * @file Process.h
 * @author Jackey Cen
 * @brief Starting another process.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mir
{
    // Quotes one argument for a Windows command line (CommandLineToArgvW rules)
    std::string QuoteCommandLineArgument(std::string_view arg);

    // Starts program with args in the current directory. With waitForExit returns its
    // exit code, otherwise 0 once started. Nullopt (logged) if it cannot start.
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit);
}
