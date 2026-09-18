/**
 * @file AssetFileIo.tests.cpp
 * @author Rahul Nair
 * @brief Tests for assets/AssetFileIo: whole-file and prefix reads/writes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <assets/AssetFileIo.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>

namespace
{
    std::filesystem::path MakeScratchFile()
    {
        static std::atomic<int> counter{0};
        return std::filesystem::temp_directory_path() /
               ("mjolnir_assetfileio_test_" + std::to_string(counter.fetch_add(1)));
    }

    std::vector<std::byte> MakeContent(std::string_view text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }
}

TEST_CASE("WriteFileBytes and ReadFileBytes round trip", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::vector<std::byte> content = MakeContent("round trip payload");

    REQUIRE(mir::WriteFileBytes(path, content));

    const std::optional<std::vector<std::byte>> read = mir::ReadFileBytes(path);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == content.size());
    CHECK(std::memcmp(read->data(), content.data(), content.size()) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("WriteFileBytes and ReadFileBytes round trip empty content", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();

    REQUIRE(mir::WriteFileBytes(path, {}));

    const std::optional<std::vector<std::byte>> read = mir::ReadFileBytes(path);
    REQUIRE(read.has_value());
    CHECK(read->empty());

    std::filesystem::remove(path);
}

TEST_CASE("ReadFileBytes returns nullopt for a missing file", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    CHECK_FALSE(mir::ReadFileBytes(path).has_value());
}

TEST_CASE("WriteFileBytes returns false when the directory does not exist", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile() / "no_such_dir" / "file.blob";
    CHECK_FALSE(mir::WriteFileBytes(path, MakeContent("data")));
}

TEST_CASE("ReadFilePrefix returns exactly the requested bytes", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteFileBytes(path, MakeContent("HEADERbody body body")));

    const std::optional<std::vector<std::byte>> prefix = mir::ReadFilePrefix(path, 6);
    REQUIRE(prefix.has_value());
    REQUIRE(prefix->size() == 6);
    CHECK(std::memcmp(prefix->data(), "HEADER", 6) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("ReadFilePrefix fails when the file is shorter than requested", "[assets][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteFileBytes(path, MakeContent("tiny")));

    CHECK_FALSE(mir::ReadFilePrefix(path, 64).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("ReadFilePrefix returns nullopt for a missing file", "[assets][fileio]")
{
    CHECK_FALSE(mir::ReadFilePrefix(MakeScratchFile(), 4).has_value());
}
