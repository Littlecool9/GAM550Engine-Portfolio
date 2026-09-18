#include <core/fs/FileIo.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

namespace
{
    .
        const std::string &ProcessToken()
    {
        static const std::string token = []
            {
                std::random_device rd;
                std::ostringstream out;
                out << std::hex << ((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
                return out.str();
            }();
        return token;
    }

    std::filesystem::path MakeScratchFile()
    {
        static std::atomic<int> counter{ 0 };
        return std::filesystem::temp_directory_path() /
            ("mjolnir_fileio_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
    }

    std::vector<std::byte> MakeContent(std::string_view text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }
}

TEST_CASE("WriteBytes and ReadBytes round trip", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::vector<std::byte> content = MakeContent("round trip payload");

    REQUIRE(mir::WriteBytes(path, content));

    const std::optional<std::vector<std::byte>> read = mir::ReadBytes(path);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == content.size());
    CHECK(std::memcmp(read->data(), content.data(), content.size()) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("WriteBytes and ReadBytes round trip empty content", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();

    REQUIRE(mir::WriteBytes(path, {}));

    const std::optional<std::vector<std::byte>> read = mir::ReadBytes(path);
    REQUIRE(read.has_value());
    CHECK(read->empty());

    std::filesystem::remove(path);
}

TEST_CASE("ReadBytes returns nullopt for a missing file", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    CHECK_FALSE(mir::ReadBytes(path).has_value());
}

TEST_CASE("WriteBytes returns false when the directory does not exist", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile() / "no_such_dir" / "file.blob";
    CHECK_FALSE(mir::WriteBytes(path, MakeContent("data")));
}

TEST_CASE("ReadFirstBytes returns exactly the requested bytes", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteBytes(path, MakeContent("HEADERbody body body")));

    const std::optional<std::vector<std::byte>> prefix = mir::ReadFirstBytes(path, 6);
    REQUIRE(prefix.has_value());
    REQUIRE(prefix->size() == 6);
    CHECK(std::memcmp(prefix->data(), "HEADER", 6) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("ReadFirstBytes fails when the file is shorter than requested", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteBytes(path, MakeContent("tiny")));

    CHECK_FALSE(mir::ReadFirstBytes(path, 64).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("ReadFirstBytes returns nullopt for a missing file", "[core][fileio]")
{
    CHECK_FALSE(mir::ReadFirstBytes(MakeScratchFile(), 4).has_value());
}

TEST_CASE("WriteText and ReadText round trip", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::string_view text = "{\n  \"key\": \"value\"\n}";

    REQUIRE(mir::WriteText(path, text));

    const std::optional<std::string> read = mir::ReadText(path);
    REQUIRE(read.has_value());
    CHECK(*read == text);

    std::filesystem::remove(path);
}

// Binary mode is the point: a '\n' must not become "\r\n" on the way out, or a
// file round trips longer than it went in.
TEST_CASE("WriteText does not translate newlines", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::string_view text = "a\nb\nc";

    REQUIRE(mir::WriteText(path, text));
    CHECK(std::filesystem::file_size(path) == text.size());

    std::filesystem::remove(path);
}

TEST_CASE("ReadText returns nullopt for a missing file", "[core][fileio]")
{
    CHECK_FALSE(mir::ReadText(MakeScratchFile()).has_value());
}

TEST_CASE("WriteBytesAtomic creates a file that was not there", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();

    REQUIRE(mir::WriteBytesAtomic(path, MakeContent("fresh")));

    const std::optional<std::string> read = mir::ReadText(path);
    REQUIRE(read.has_value());
    CHECK(*read == "fresh");

    std::filesystem::remove(path);
}

TEST_CASE("WriteBytesAtomic replaces an existing file", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, "the old and longer contents"));

    REQUIRE(mir::WriteBytesAtomic(path, MakeContent("new")));

    const std::optional<std::string> read = mir::ReadText(path);
    REQUIRE(read.has_value());
    CHECK(*read == "new");

    std::filesystem::remove(path);
}

// The temp file is a sibling, so a failed write must not leave it lying next to
// the real one for the next reader to trip over.
TEST_CASE("WriteBytesAtomic leaves no temp file behind on success", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteBytesAtomic(path, MakeContent("payload")));

    std::filesystem::path temp = path;
    temp += ".tmp";
    CHECK_FALSE(std::filesystem::exists(temp));

    std::filesystem::remove(path);
}

TEST_CASE("WriteBytesAtomic fails when the directory does not exist", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile() / "no_such_dir" / "file.json";
    CHECK_FALSE(mir::WriteBytesAtomic(path, MakeContent("data")));
}

TEST_CASE("WriteTextAtomic round trips", "[core][fileio]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteTextAtomic(path, "atomic text"));

    const std::optional<std::string> read = mir::ReadText(path);
    REQUIRE(read.has_value());
    CHECK(*read == "atomic text");

    std::filesystem::remove(path);
}
