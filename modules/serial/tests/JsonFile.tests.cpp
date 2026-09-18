#include <serial/JsonFile.h>

#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace
{
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
            ("mjolnir_jsonfile_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)) + ".json");
    }
}

TEST_CASE("ReadJsonFile parses an object", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, R"({"a": 1, "b": "two"})"));

    const std::optional<nlohmann::json> root = mir::ReadJsonFile(path);
    REQUIRE(root.has_value());
    CHECK(root->value("a", 0) == 1);
    CHECK(root->value("b", std::string()) == "two");

    std::filesystem::remove(path);
}

TEST_CASE("ReadJsonFile returns nullopt for a missing file", "[serial][jsonFile]")
{
    CHECK_FALSE(mir::ReadJsonFile(MakeScratchFile()).has_value());
}

TEST_CASE("ReadJsonFile returns nullopt for malformed JSON", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, "{not json"));

    CHECK_FALSE(mir::ReadJsonFile(path).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("WriteJsonFile and ReadJsonFile round trip", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    const nlohmann::json written{ { "rendering",{ { "rayTracing", true } } } };

    REQUIRE(mir::WriteJsonFile(path, written));

    const std::optional<nlohmann::json> read = mir::ReadJsonFile(path);
    REQUIRE(read.has_value());
    CHECK(*read == written);

    std::filesystem::remove(path);
}

TEST_CASE("WriteJsonFile honours the indent argument", "[serial][jsonFile]")
{
    const std::filesystem::path compact = MakeScratchFile();
    const std::filesystem::path pretty = MakeScratchFile();
    const nlohmann::json root{ { "a", 1 },{ "b", 2 } };

    REQUIRE(mir::WriteJsonFile(compact, root, -1));
    REQUIRE(mir::WriteJsonFile(pretty, root, 2));

    CHECK(std::filesystem::file_size(compact) < std::filesystem::file_size(pretty));

    std::filesystem::remove(compact);
    std::filesystem::remove(pretty);
}

TEST_CASE("UpdateJsonFile creates a file that was not there", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();

    REQUIRE(mir::UpdateJsonFile(path, [](nlohmann::json &root)
        {
            root["input"] = nlohmann::json::object();
        }));

    const std::optional<nlohmann::json> read = mir::ReadJsonFile(path);
    REQUIRE(read.has_value());
    CHECK(read->contains("input"));

    std::filesystem::remove(path);
}

// The point of the whole layer: two owners of two sections of one file, neither
// knowing about the other.
TEST_CASE("UpdateJsonFile leaves sections it did not touch alone", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, R"({"input": {"actions": []}, "handWritten": 7})"));

    REQUIRE(mir::UpdateJsonFile(path, [](nlohmann::json &root)
        {
            root["rendering"] = nlohmann::json{ { "rayTracing", true } };
        }));

    const std::optional<nlohmann::json> read = mir::ReadJsonFile(path);
    REQUIRE(read.has_value());
    CHECK(read->at("rendering").at("rayTracing").get<bool>());
    CHECK(read->contains("input"));
    CHECK(read->value("handWritten", 0) == 7);

    std::filesystem::remove(path);
}

// A typo in a hand-edited settings file must not cost the user every other
// section in it.
TEST_CASE("UpdateJsonFile refuses to overwrite a malformed file", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::string_view original = R"({"input": broken)";
    REQUIRE(mir::WriteText(path, original));

    CHECK_FALSE(mir::UpdateJsonFile(path, [](nlohmann::json &root)
        {
            root["rendering"] = true;
        }));

    const std::optional<std::string> onDisk = mir::ReadText(path);
    REQUIRE(onDisk.has_value());
    CHECK(*onDisk == original); // untouched

    std::filesystem::remove(path);
}

TEST_CASE("UpdateJsonFile refuses a file whose root is not an object", "[serial][jsonFile]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, "[1, 2, 3]"));

    CHECK_FALSE(mir::UpdateJsonFile(path, [](nlohmann::json &root)
        {
            root["rendering"] = true;
        }));

    const std::optional<std::string> onDisk = mir::ReadText(path);
    REQUIRE(onDisk.has_value());
    CHECK(*onDisk == "[1, 2, 3]");

    std::filesystem::remove(path);
}
