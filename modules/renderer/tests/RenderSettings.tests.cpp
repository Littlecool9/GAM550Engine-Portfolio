/**
 * @file RenderSettings.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/RenderSettings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    struct TempSettingsFile
    {
        std::filesystem::path mPath;

        explicit TempSettingsFile(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_render_settings_tests" / name)
        {
            std::filesystem::create_directories(mPath.parent_path());
            std::filesystem::remove(mPath);
        }

        void Write(std::string_view contents) const
        {
            std::ofstream out(mPath);
            out << contents;
        }

        std::string Read() const
        {
            std::ifstream in(mPath);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        ~TempSettingsFile() { std::filesystem::remove(mPath); }
    };
}

TEST_CASE("LoadFile on a missing path returns defaults", "[renderer]")
{
    const mir::RenderSettings settings = mir::RenderSettings::LoadFile("no/such/project_settings.json");
    REQUIRE_FALSE(settings.rayTracing);
}

TEST_CASE("LoadFile on a legacy unsectioned input-only file returns defaults", "[renderer]")
{
    TempSettingsFile file{"legacy_unsectioned.json"};
    file.Write(R"({"actions": []})");

    const mir::RenderSettings settings = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE_FALSE(settings.rayTracing);
}

TEST_CASE("SaveFile then LoadFile round-trips rayTracing", "[renderer]")
{
    TempSettingsFile file{"roundtrip.json"};

    mir::RenderSettings saved;
    saved.rayTracing = true;
    REQUIRE(saved.SaveFile(file.mPath));

    const mir::RenderSettings loaded = mir::RenderSettings::LoadFile(file.mPath);
    REQUIRE(loaded.rayTracing);
}

TEST_CASE("SaveFile preserves a sibling 'input' section instead of clobbering it", "[renderer]")
{
    TempSettingsFile file{"sibling_preserved.json"};
    file.Write(R"({"input": {"actions": [{"name": "ZZZ_MARKER_ACTION", "type": "Button", "bindings": []}]}})");

    mir::RenderSettings saved;
    saved.rayTracing = true;
    REQUIRE(saved.SaveFile(file.mPath));

    const std::string contents = file.Read();
    REQUIRE(contents.find("ZZZ_MARKER_ACTION") != std::string::npos);
    REQUIRE(contents.find("\"rayTracing\": true") != std::string::npos);
}
