/**
 * @file InputMap.cpp
 * @author Rahul Nair
 * @brief The authored action -> binding table (Project Settings' input
 *        section). A World resource - InputSystem reads it every frame, so
 *        editing it live (e.g. from a future rebind UI) takes effect at once.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "input/InputMap.h"

#include <core/log/Log.h>

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        std::string_view ToString(DeviceKind device)
        {
            switch (device)
            {
            case DeviceKind::Keyboard:
                return "Keyboard";
            case DeviceKind::Mouse:
                return "Mouse";
            case DeviceKind::Gamepad:
                return "Gamepad";
            }
            return "Keyboard";
        }

        DeviceKind ParseDeviceKind(std::string_view name)
        {
            if (name == "Mouse")
                return DeviceKind::Mouse;
            if (name == "Gamepad")
                return DeviceKind::Gamepad;
            return DeviceKind::Keyboard;
        }

        std::string_view ToString(AxisChannel channel)
        {
            return channel == AxisChannel::Y ? "Y" : "X";
        }

        AxisChannel ParseAxisChannel(std::string_view name)
        {
            return name == "Y" ? AxisChannel::Y : AxisChannel::X;
        }

        json ToJson(const InputBinding &binding)
        {
            return json{
                { "device", ToString(binding.device) },
                { "channel", ToString(binding.channel) },
                { "key", KeyName(binding.key) },
                { "mouseButton", MouseButtonName(binding.mouseButton) },
                { "useGamepadAxis", binding.useGamepadAxis },
                { "gamepadButton", GamepadButtonName(binding.gamepadButton) },
                { "gamepadAxis", GamepadAxisName(binding.gamepadAxis) },
                { "deadzone", binding.deadzone },
                { "scale", binding.scale },
            };
        }

        InputBinding BindingFromJson(const json &node)
        {
            InputBinding binding;
            binding.device = ParseDeviceKind(node.value("device", std::string("Keyboard")));
            binding.channel = ParseAxisChannel(node.value("channel", std::string("X")));
            binding.key = ParseKey(node.value("key", std::string()));
            binding.mouseButton = ParseMouseButton(node.value("mouseButton", std::string())).value_or(MouseButton::Left);
            binding.useGamepadAxis = node.value("useGamepadAxis", false);
            binding.gamepadButton = ParseGamepadButton(node.value("gamepadButton", std::string())).value_or(GamepadButton::A);
            binding.gamepadAxis = ParseGamepadAxis(node.value("gamepadAxis", std::string())).value_or(GamepadAxis::LeftX);
            binding.deadzone = node.value("deadzone", 0.15f);
            binding.scale = node.value("scale", 1.0f);
            return binding;
        }
    }

    std::string_view ActionTypeName(InputActionType type)
    {
        switch (type)
        {
        case InputActionType::Button:
            return "Button";
        case InputActionType::Axis1D:
            return "Axis1D";
        case InputActionType::Axis2D:
            return "Axis2D";
        }
        return "Button";
    }

    InputActionType ParseActionType(std::string_view name)
    {
        if (name == "Axis1D")
            return InputActionType::Axis1D;
        if (name == "Axis2D")
            return InputActionType::Axis2D;
        return InputActionType::Button;
    }

    InputAction &InputMap::AddAction(std::string name, InputActionType type)
    {
        if (InputAction *existing = Find(name))
            return *existing;

        InputAction &action = mActions.emplace_back();
        action.name = std::move(name);
        action.type = type;
        return action;
    }

    bool InputMap::RemoveAction(std::string_view name)
    {
        const auto it = std::find_if(mActions.begin(), mActions.end(),
            [&](const InputAction &action)
            {
                return action.name == name;
            });
        if (it == mActions.end())
            return false;

        mActions.erase(it);
        return true;
    }

    InputAction *InputMap::Find(std::string_view name)
    {
        const auto it = std::find_if(mActions.begin(), mActions.end(),
            [&](const InputAction &action)
            {
                return action.name == name;
            });
        return it != mActions.end() ? &*it : nullptr;
    }

    const InputAction *InputMap::Find(std::string_view name) const
    {
        return const_cast<InputMap *>(this)->Find(name);
    }

    std::optional<InputMap> InputMap::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return std::nullopt;

        InputMap map;

        // Falls back to the document root so a file written before the sectioned
        // schema (actions at top level) still loads.
        const auto inputIt = root->find("input");
        const json &inputRoot = inputIt != root->end() ? *inputIt : *root;

        // json::value() throws type_error 306 on a non-object receiver, so every
        // node is checked before it is read. A junk section is a content error in
        // one part of a readable file, so it loads as empty rather than nullopt.
        if (!inputRoot.is_object())
        {
            MIR_LOG_ERROR("InputMap::LoadFile: '{}' has an \"input\" value that is not an object", path.string());
            return map;
        }

        for (const json &actionNode : inputRoot.value("actions", json::array()))
        {
            if (!actionNode.is_object())
            {
                MIR_LOG_WARN("InputMap::LoadFile: '{}' has a non-object action, skipped", path.string());
                continue;
            }

            InputAction &action = map.AddAction(
                actionNode.value("name", ""),
                ParseActionType(actionNode.value("type", std::string("Button"))));

            for (const json &bindingNode : actionNode.value("bindings", json::array()))
            {
                if (!bindingNode.is_object())
                {
                    MIR_LOG_WARN("InputMap::LoadFile: '{}' has a non-object binding, skipped", path.string());
                    continue;
                }
                action.bindings.push_back(BindingFromJson(bindingNode));
            }
        }

        return map;
    }

    bool InputMap::SaveFile(const std::filesystem::path &path) const
    {
        // Only the "input" key is written; UpdateJsonFile carries the rest of
        // project_settings.json through untouched.
        return UpdateJsonFile(path, [this](json &root)
            {
                // a file loaded through the fallback above is migrated on its first 
                // save instead of keeping two copies of the same actions. 
                root.erase("actions");

                json inputNode;
                json &actions = inputNode["actions"] = json::array();

                for (const InputAction &action : mActions)
                {
                    json actionNode{
                        { "name", action.name },
                        { "type", ActionTypeName(action.type) },
                        { "bindings", json::array() },
                    };

                    for (const InputBinding &binding : action.bindings)
                        actionNode["bindings"].push_back(ToJson(binding));

                    actions.push_back(std::move(actionNode));
                }

                root["input"] = std::move(inputNode);
            });
    }
}
