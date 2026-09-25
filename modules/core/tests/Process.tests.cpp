/**
 * @file Process.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/platform/Process and ExecutablePath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/Paths.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

TEST_CASE("ExecutablePath is a file inside ExecutableDir", "[process]")
{
    const std::filesystem::path &path = mir::ExecutablePath();
    REQUIRE(std::filesystem::is_regular_file(path));
    CHECK(path.parent_path() == mir::ExecutableDir());
}

TEST_CASE("QuoteCommandLineArgument leaves a plain argument alone", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument("--game") == "--game");
    CHECK(mir::QuoteCommandLineArgument("games/HelloWorld") == "games/HelloWorld");
    CHECK(mir::QuoteCommandLineArgument(R"(C:\no\spaces\)") == R"(C:\no\spaces\)");
}

TEST_CASE("QuoteCommandLineArgument quotes an empty argument and whitespace", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument("") == R"("")");
    CHECK(mir::QuoteCommandLineArgument("games/My Game") == R"("games/My Game")");
}

TEST_CASE("QuoteCommandLineArgument escapes quotes and the backslashes around them", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument(R"(say "hi")") == R"("say \"hi\"")");
    CHECK(mir::QuoteCommandLineArgument(R"(a\"b)") == R"("a\\\"b")");
    CHECK(mir::QuoteCommandLineArgument(R"(C:\My Dir\)") == R"("C:\My Dir\\")");
}

TEST_CASE("LaunchProcess returns the exit code of a process it waits for", "[process]")
{
#if defined(_WIN32)
    char *comspec = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
    REQUIRE(comspec != nullptr);
    const std::filesystem::path shell = comspec;
    std::free(comspec);
    const std::vector<std::string> args{"/c", "exit 3"};
#else
    const std::filesystem::path shell = "/bin/sh";
    const std::vector<std::string> args{"-c", "exit 3"};
#endif

    const std::optional<int> exitCode = mir::LaunchProcess(shell, args, true);
    REQUIRE(exitCode.has_value());
    CHECK(*exitCode == 3);
}

TEST_CASE("LaunchProcess reports a program that does not exist", "[process]")
{
    const std::vector<std::string> args;
    CHECK_FALSE(mir::LaunchProcess(mir::ExecutableDir() / "no_such_program.exe", args, true).has_value());
}

TEST_CASE("ChildProcess starts on demand and exposes its eventual exit code", "[process]")
{
#if defined(_WIN32)
    char *comspec = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
    REQUIRE(comspec != nullptr);
    const std::filesystem::path shell = comspec;
    std::free(comspec);
    const std::vector<std::string> args{"/c", "exit 7"};
#else
    const std::filesystem::path shell = "/bin/sh";
    const std::vector<std::string> args{"-c", "exit 7"};
#endif
    mir::ChildProcess child;
    REQUIRE(child.Start(shell, args));
    const auto exitCode = child.Wait();
    REQUIRE(exitCode);
    CHECK(*exitCode == 7);
    CHECK_FALSE(child.Running());
}
