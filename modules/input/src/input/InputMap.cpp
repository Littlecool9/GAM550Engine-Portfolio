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

#include <assets/AssetFileIo.h>
#include <core/log/Log.h>

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
                {"device", ToString(binding.device)},
                {"channel", ToString(binding.channel)},
                {"key", KeyName(binding.key)},
                {"mouseButton", MouseButtonName(binding.mouseButton)},
                {"useGamepadAxis", binding.useGamepadAxis},
                {"gamepadButton", GamepadButtonName(binding.gamepadButton)},
                {"gamepadAxis", GamepadAxisName(binding.gamepadAxis)},
                {"deadzone", binding.deadzone},
                {"scale", binding.scale},
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
                                      { return action.name == name; });
        if (it == mActions.end())
            return false;

        mActions.erase(it);
        return true;
    }

    InputAction *InputMap::Find(std::string_view name)
    {
        const auto it = std::find_if(mActions.begin(), mActions.end(),
                                      [&](const InputAction &action)
                                      { return action.name == name; });
        return it != mActions.end() ? &*it : nullptr;
    }

    const InputAction *InputMap::Find(std::string_view name) const
    {
        return const_cast<InputMap *>(this)->Find(name);
    }

    InputMap InputMap::LoadFile(const std::filesystem::path &path)
    {
        InputMap map;

        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError) && !existsError)
            return map;

        const std::optional<std::vector<std::byte>> bytes = ReadFileBytes(path);
        if (!bytes)
            return map;

        json root;
        try
        {
            root = json::parse(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
        }
        catch (const json::parse_error &e)
        {
            MIR_LOG_ERROR("InputMap::LoadFile: '{}' is not valid JSON ({})", path.string(), e.what());
            return map;
        }

        const auto inputIt = root.find("input");
        const json &inputRoot = inputIt != root.end() ? *inputIt : root;

        for (const json &actionNode : inputRoot.value("actions", json::array()))
        {
            InputAction &action = map.AddAction(
                actionNode.value("name", ""),
                ParseActionType(actionNode.value("type", std::string("Button"))));

            for (const json &bindingNode : actionNode.value("bindings", json::array()))
                action.bindings.push_back(BindingFromJson(bindingNode));
        }

        return map;
    }

    bool InputMap::SaveFile(const std::filesystem::path &path) const
    {
        json root = json::object();
        if (const std::optional<std::vector<std::byte>> bytes = ReadFileBytes(path))
        {
            try
            {
                json existing = json::parse(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
                if (existing.is_object())
                    root = std::move(existing);
            }
            catch (const json::parse_error &)
            {
            }
        }
        root.erase("actions");

        json inputNode;
        json &actions = inputNode["actions"] = json::array();

        for (const InputAction &action : mActions)
        {
            json actionNode{
                {"name", action.name},
                {"type", ActionTypeName(action.type)},
                {"bindings", json::array()},
            };

            for (const InputBinding &binding : action.bindings)
                actionNode["bindings"].push_back(ToJson(binding));

            actions.push_back(std::move(actionNode));
        }

        root["input"] = std::move(inputNode);

        const std::string dumped = root.dump(2);
        const std::span<const std::byte> bytesOut(reinterpret_cast<const std::byte *>(dumped.data()), dumped.size());
        return WriteFileBytes(path, bytesOut);
    }
}
