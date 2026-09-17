#include "input/InputMap.h"

#include <core/log/Log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

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

        json ReadRoot(const std::filesystem::path &path)
        {
            if (!std::filesystem::exists(path))
                return json::object();

            std::ifstream in(path);
            if (!in)
                return json::object();

            json root;
            try
            {
                in >> root;
            }
            catch (const json::parse_error &)
            {
                return json::object();
            }

            return root.is_object() ? root : json::object();
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

        if (!std::filesystem::exists(path))
        {
            MIR_LOG_INFO("InputMap::LoadFile: no '{}' yet, starting with an empty input map", path.string());
            return map;
        }

        std::ifstream in(path);
        if (!in)
        {
            MIR_LOG_ERROR("InputMap::LoadFile: could not open '{}'", path.string());
            return map;
        }

        json root;
        try
        {
            in >> root;
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
        json root = ReadRoot(path);
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

        std::ofstream out(path);
        if (!out)
        {
            MIR_LOG_ERROR("InputMap::SaveFile: could not open '{}' for writing", path.string());
            return false;
        }

        out << root.dump(2);
        if (!out)
        {
            MIR_LOG_ERROR("InputMap::SaveFile: write to '{}' failed", path.string());
            return false;
        }

        return true;
    }
}
