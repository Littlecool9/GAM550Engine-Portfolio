/**
 * @file InputMap.tests.cpp
 * @author Rahul Nair
 * @brief Tests for input/InputMap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/InputMap.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    struct TempInputMapFile
    {
        std::filesystem::path mPath;

        explicit TempInputMapFile(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_input_tests" / name)
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

        ~TempInputMapFile() { std::filesystem::remove(mPath); }
    };
}

TEST_CASE("AddAction then Find returns the same action", "[input]")
{
    mir::InputMap map;
    map.AddAction("Jump", mir::InputActionType::Button);

    const mir::InputAction *found = map.Find("Jump");
    REQUIRE(found != nullptr);
    REQUIRE(found->name == "Jump");
    REQUIRE(found->type == mir::InputActionType::Button);
}

TEST_CASE("AddAction with an existing name returns the existing action instead of duplicating it", "[input]")
{
    mir::InputMap map;
    mir::InputAction &first = map.AddAction("Jump", mir::InputActionType::Button);
    first.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Keyboard, .key = mir::Key::Space });

    mir::InputAction &second = map.AddAction("Jump", mir::InputActionType::Axis1D);

    REQUIRE(&second == &first);
    REQUIRE(map.Actions().size() == 1);
    REQUIRE(second.type == mir::InputActionType::Button); // unchanged - not overwritten by the second call
    REQUIRE(second.bindings.size() == 1);
}

TEST_CASE("Find on an unknown action returns nullptr", "[input]")
{
    mir::InputMap map;
    REQUIRE(map.Find("NoSuchAction") == nullptr);
}

TEST_CASE("RemoveAction removes exactly the named action", "[input]")
{
    mir::InputMap map;
    map.AddAction("Jump", mir::InputActionType::Button);
    map.AddAction("Move", mir::InputActionType::Axis1D);

    REQUIRE(map.RemoveAction("Jump"));
    REQUIRE(map.Find("Jump") == nullptr);
    REQUIRE(map.Find("Move") != nullptr);
    REQUIRE_FALSE(map.RemoveAction("Jump"));
}

TEST_CASE("LoadFile on a missing path returns nullopt", "[input]")
{
    CHECK_FALSE(mir::InputMap::LoadFile("no/such/project_settings.json").has_value());
}

// A file that exists but has no "input" section is a different answer from no
// file at all: the caller gets an empty map, not nullopt.
TEST_CASE("LoadFile on a file with no input section returns an empty map", "[input]")
{
    TempInputMapFile file{ "no_input_section.json" };
    file.Write(R"({"rendering": {"rayTracing": true}})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    CHECK(map->Actions().empty());
}

TEST_CASE("LoadFile on a malformed file returns nullopt", "[input]")
{
    TempInputMapFile file{ "malformed.json" };
    file.Write(R"({"input": broken)");

    CHECK_FALSE(mir::InputMap::LoadFile(file.mPath).has_value());
}

TEST_CASE("LoadFile reads a legacy unsectioned file back-compat", "[input]")
{
    TempInputMapFile file{ "legacy_unsectioned.json" };
    file.Write(R"({"actions": [{"name": "Jump", "type": "Button", "bindings": []}]})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    REQUIRE(map->Find("Jump") != nullptr);
}

TEST_CASE("LoadFile reads the sectioned 'input' key and ignores a sibling 'rendering' key", "[input]")
{
    TempInputMapFile file{ "sectioned.json" };
    file.Write(R"({"input": {"actions": [{"name": "Jump", "type": "Button", "bindings": []}]}, "rendering": {"rayTracing": true}})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    REQUIRE(map->Find("Jump") != nullptr);
    REQUIRE(map->Actions().size() == 1);
}

// json::value() throws type_error 306 on a non-object receiver, and nothing on
// the startup path catches it. Each of these three used to terminate the process.
TEST_CASE("LoadFile survives an input key that is not an object", "[input]")
{
    TempInputMapFile file{ "input_not_object.json" };
    file.Write(R"({"input": null})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    CHECK(map->Actions().empty());
}

TEST_CASE("LoadFile skips a non-object action", "[input]")
{
    TempInputMapFile file{ "action_not_object.json" };
    file.Write(R"({"input": {"actions": [7, {"name": "Jump", "type": "Button", "bindings": []}]}})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    CHECK(map->Actions().size() == 1);
    CHECK(map->Find("Jump") != nullptr);
}

TEST_CASE("LoadFile skips a non-object binding", "[input]")
{
    TempInputMapFile file{ "binding_not_object.json" };
    file.Write(R"({"input": {"actions": [{"name": "Jump", "type": "Button", "bindings": ["nope"]}]}})");

    const std::optional<mir::InputMap> map = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(map.has_value());
    REQUIRE(map->Find("Jump") != nullptr);
    CHECK(map->Find("Jump")->bindings.empty());
}

TEST_CASE("SaveFile preserves a sibling 'rendering' section instead of clobbering it", "[input]")
{
    TempInputMapFile file{ "sibling_preserved.json" };
    file.Write(R"({"rendering": {"rayTracing": true}})");

    mir::InputMap saved;
    saved.AddAction("Jump", mir::InputActionType::Button);
    REQUIRE(saved.SaveFile(file.mPath));

    const std::string contents = file.Read();
    REQUIRE(contents.find("\"rayTracing\": true") != std::string::npos);

    const std::optional<mir::InputMap> loaded = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->Find("Jump") != nullptr);
}

TEST_CASE("SaveFile then LoadFile round-trips a Button action with mixed-device bindings", "[input]")
{
    TempInputMapFile file{ "button_roundtrip.json" };

    mir::InputMap saved;
    mir::InputAction &jump = saved.AddAction("Jump", mir::InputActionType::Button);
    jump.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Keyboard, .key = mir::Key::Space });
    mir::InputBinding padBinding{ .device = mir::DeviceKind::Gamepad };
    padBinding.gamepadButton = mir::GamepadButton::A;
    jump.bindings.push_back(padBinding);

    REQUIRE(saved.SaveFile(file.mPath));

    const std::optional<mir::InputMap> loaded = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    const mir::InputAction *loadedJump = loaded->Find("Jump");
    REQUIRE(loadedJump != nullptr);
    REQUIRE(loadedJump->type == mir::InputActionType::Button);
    REQUIRE(loadedJump->bindings.size() == 2);

    REQUIRE(loadedJump->bindings[0].device == mir::DeviceKind::Keyboard);
    REQUIRE(loadedJump->bindings[0].key == mir::Key::Space);

    REQUIRE(loadedJump->bindings[1].device == mir::DeviceKind::Gamepad);
    REQUIRE(loadedJump->bindings[1].gamepadButton == mir::GamepadButton::A);
}

TEST_CASE("SaveFile then LoadFile round-trips an Axis1D action's scale and an analog gamepad axis", "[input]")
{
    TempInputMapFile file{ "axis1d_roundtrip.json" };

    mir::InputMap saved;
    mir::InputAction &move = saved.AddAction("Move", mir::InputActionType::Axis1D);
    move.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Keyboard, .key = mir::Key::D, .scale = 1.0f });
    move.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Keyboard, .key = mir::Key::A, .scale = -1.0f });

    mir::InputBinding stick{ .device = mir::DeviceKind::Gamepad };
    stick.useGamepadAxis = true;
    stick.gamepadAxis = mir::GamepadAxis::LeftX;
    stick.deadzone = 0.25f;
    move.bindings.push_back(stick);

    REQUIRE(saved.SaveFile(file.mPath));

    const std::optional<mir::InputMap> loaded = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    const mir::InputAction *loadedMove = loaded->Find("Move");
    REQUIRE(loadedMove != nullptr);
    REQUIRE(loadedMove->type == mir::InputActionType::Axis1D);
    REQUIRE(loadedMove->bindings.size() == 3);

    REQUIRE(loadedMove->bindings[0].key == mir::Key::D);
    REQUIRE(loadedMove->bindings[0].scale == 1.0f);
    REQUIRE(loadedMove->bindings[1].key == mir::Key::A);
    REQUIRE(loadedMove->bindings[1].scale == -1.0f);

    REQUIRE(loadedMove->bindings[2].useGamepadAxis);
    REQUIRE(loadedMove->bindings[2].gamepadAxis == mir::GamepadAxis::LeftX);
    REQUIRE(loadedMove->bindings[2].deadzone == 0.25f);
}

TEST_CASE("SaveFile then LoadFile round-trips an Axis2D action's per-binding channel", "[input]")
{
    TempInputMapFile file{ "axis2d_roundtrip.json" };

    mir::InputMap saved;
    mir::InputAction &look = saved.AddAction("Look", mir::InputActionType::Axis2D);
    look.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Gamepad,
                                               .channel = mir::AxisChannel::X,
                                               .useGamepadAxis = true,
                                               .gamepadAxis = mir::GamepadAxis::RightX });
    look.bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Gamepad,
                                               .channel = mir::AxisChannel::Y,
                                               .useGamepadAxis = true,
                                               .gamepadAxis = mir::GamepadAxis::RightY });

    REQUIRE(saved.SaveFile(file.mPath));

    const std::optional<mir::InputMap> loaded = mir::InputMap::LoadFile(file.mPath);
    REQUIRE(loaded.has_value());
    const mir::InputAction *loadedLook = loaded->Find("Look");
    REQUIRE(loadedLook != nullptr);
    REQUIRE(loadedLook->type == mir::InputActionType::Axis2D);
    REQUIRE(loadedLook->bindings.size() == 2);

    REQUIRE(loadedLook->bindings[0].channel == mir::AxisChannel::X);
    REQUIRE(loadedLook->bindings[0].gamepadAxis == mir::GamepadAxis::RightX);
    REQUIRE(loadedLook->bindings[1].channel == mir::AxisChannel::Y);
    REQUIRE(loadedLook->bindings[1].gamepadAxis == mir::GamepadAxis::RightY);
}
