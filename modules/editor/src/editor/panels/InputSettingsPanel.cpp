/**
 * @file InputSettingsPanel.cpp
 * @author Rahul Nair
 * @brief The "Input Settings" window - add/remove actions, add/remove/rebind
 *        bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/InputSettingsPanel.h>

#include <imgui.h>

namespace mir
{
    namespace
    {
        bool AnyGamepadButtonJustPressed(const RawInputSnapshot &raw, const RawInputSnapshot &previous,
            GamepadButton &outButton)
        {
            for (size_t slot = 0; slot < raw.gamepads.size(); ++slot)
            {
                const GamepadState &pad = raw.gamepads[slot];
                if (!pad.connected)
                    continue;

                const GamepadState &prevPad = previous.gamepads[slot];
                for (int b = 0; b < kGamepadButtonCount; ++b)
                {
                    if (pad.buttons[static_cast<size_t>(b)] && !prevPad.buttons[static_cast<size_t>(b)])
                    {
                        outButton = static_cast<GamepadButton>(b);
                        return true;
                    }
                }
            }
            return false;
        }
    }

    bool InputSettingsPanel::TryCaptureBinding(InputBinding &binding, const RawInputSnapshot &raw)
    {
        if (raw.IsDown(Key::Escape) && !mPreviousRaw.IsDown(Key::Escape))
            return true;

        for (int i = 0; i < kKeyCount; ++i)
        {
            if (raw.keys[static_cast<size_t>(i)] && !mPreviousRaw.keys[static_cast<size_t>(i)])
            {
                binding.device = DeviceKind::Keyboard;
                binding.key = static_cast<Key>(i);
                return true;
            }
        }

        for (int i = 0; i < kMouseButtonCount; ++i)
        {
            if (raw.mouseButtons[static_cast<size_t>(i)] && !mPreviousRaw.mouseButtons[static_cast<size_t>(i)])
            {
                binding.device = DeviceKind::Mouse;
                binding.mouseButton = static_cast<MouseButton>(i);
                return true;
            }
        }

        if (GamepadButton pressed{}; AnyGamepadButtonJustPressed(raw, mPreviousRaw, pressed))
        {
            binding.device = DeviceKind::Gamepad;
            binding.useGamepadAxis = false;
            binding.gamepadButton = pressed;
            return true;
        }

        return false;
    }

    bool InputSettingsPanel::DrawBinding(const InputAction &action, InputBinding &binding, int actionIndex,
        int bindingIndex, const RawInputSnapshot &raw)
    {
        ImGui::PushID(bindingIndex);

        const bool listening = mListeningAction == actionIndex && mListeningBinding == bindingIndex;
        if (listening)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Press any key/button... (Esc to cancel)");
            if (TryCaptureBinding(binding, raw))
            {
                mListeningAction = -1;
                mListeningBinding = -1;
            }
            ImGui::PopID();
            return false;
        }

        ImGui::SetNextItemWidth(120.0f);
        int deviceIndex = static_cast<int>(binding.device);
        const char *deviceItems[] = { "Keyboard", "Mouse", "Gamepad" };
        if (ImGui::Combo("##Device", &deviceIndex, deviceItems, IM_ARRAYSIZE(deviceItems)))
        {
            binding.device = static_cast<DeviceKind>(deviceIndex);
            mListeningAction = actionIndex;
            mListeningBinding = bindingIndex;
            mPreviousRaw = raw;
            ImGui::PopID();
            return false;
        }

        ImGui::SameLine();

        if (binding.device == DeviceKind::Gamepad)
        {
            ImGui::Checkbox("Analog", &binding.useGamepadAxis);
            ImGui::SameLine();
        }

        const bool isAnalog = binding.device == DeviceKind::Gamepad && binding.useGamepadAxis;

        if (isAnalog)
        {
            const std::string axisLabel(GamepadAxisName(binding.gamepadAxis));
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo("##Axis", axisLabel.c_str()))
            {
                for (int i = 0; i < kGamepadAxisCount; ++i)
                {
                    const auto axis = static_cast<GamepadAxis>(i);
                    const std::string itemLabel(GamepadAxisName(axis));
                    if (ImGui::Selectable(itemLabel.c_str(), binding.gamepadAxis == axis))
                        binding.gamepadAxis = axis;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("Deadzone", &binding.deadzone, 0.01f, 0.0f, 0.9f);
        }
        else
        {
            std::string label;
            if (binding.device == DeviceKind::Keyboard)
                label = KeyName(binding.key);
            else if (binding.device == DeviceKind::Mouse)
                label = MouseButtonName(binding.mouseButton);
            else
                label = GamepadButtonName(binding.gamepadButton);

            ImGui::TextUnformatted(!label.empty() ? label.c_str() : "(unbound)");
            ImGui::SameLine();
            if (ImGui::Button("Rebind"))
            {
                mListeningAction = actionIndex;
                mListeningBinding = bindingIndex;
                mPreviousRaw = raw;
            }
        }

        if (action.type != InputActionType::Button && !isAnalog)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::DragFloat("Scale", &binding.scale, 0.1f, -1.0f, 1.0f);
        }

        if (action.type == InputActionType::Axis2D)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(55.0f);
            int channelIndex = static_cast<int>(binding.channel);
            const char *channelItems[] = { "X", "Y" };
            if (ImGui::Combo("##Channel", &channelIndex, channelItems, IM_ARRAYSIZE(channelItems)))
                binding.channel = static_cast<AxisChannel>(channelIndex);
        }

        ImGui::SameLine();
        const bool removeRequested = ImGui::SmallButton("X");

        ImGui::PopID();
        return removeRequested;
    }

    bool InputSettingsPanel::DrawAction(InputAction &action, int actionIndex, const RawInputSnapshot &raw)
    {
        ImGui::PushID(actionIndex);

        bool removeAction = false;

        if (ImGui::CollapsingHeader(action.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            ImGui::TextDisabled("%s", ActionTypeName(action.type).data());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove Action"))
                removeAction = true;

            int removeBindingIndex = -1;
            for (int i = 0; i < static_cast<int>(action.bindings.size()); ++i)
            {
                if (DrawBinding(action, action.bindings[static_cast<size_t>(i)], actionIndex, i, raw))
                    removeBindingIndex = i;
            }
            if (removeBindingIndex >= 0)
            {
                action.bindings.erase(action.bindings.begin() + removeBindingIndex);

                if (mListeningAction == actionIndex)
                    CancelListening();
            }

            if (ImGui::SmallButton("+ Binding"))
            {
                action.bindings.push_back(InputBinding{});
                mListeningAction = actionIndex;
                mListeningBinding = static_cast<int>(action.bindings.size()) - 1;
                mPreviousRaw = raw;
            }

            ImGui::Unindent();
        }

        ImGui::PopID();
        return removeAction;
    }

    void InputSettingsPanel::Draw(InputMap &inputMap, const RawInputSnapshot &raw,
        const std::filesystem::path &savePath)
    {
        if (!mDraftLoaded)
        {
            mDraftInputMap = inputMap;
            mDraftLoaded = true;
        }

        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputTextWithHint("##NewActionName", "New action name...", mNewActionName, sizeof(mNewActionName));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        const char *typeItems[] = { "Button", "Axis1D", "Axis2D" };
        ImGui::Combo("##NewActionType", &mNewActionType, typeItems, IM_ARRAYSIZE(typeItems));

        ImGui::SameLine();
        const bool canAdd = mNewActionName[0] != '\0' && mDraftInputMap.Find(mNewActionName) == nullptr;
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button("+ Action"))
        {
            mDraftInputMap.AddAction(mNewActionName, static_cast<InputActionType>(mNewActionType));
            mNewActionName[0] = '\0';
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            inputMap = mDraftInputMap;
            mStatusMessage = inputMap.SaveFile(savePath) ? "Saved." : "Save failed - see log.";
        }

        if (!mStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mStatusMessage.c_str());
        }

        ImGui::Separator();

        std::vector<InputAction> &actions = mDraftInputMap.MutableActions();
        std::string pendingRemove;
        for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        {
            if (DrawAction(actions[static_cast<size_t>(i)], i, raw))
                pendingRemove = actions[static_cast<size_t>(i)].name;
        }
        if (!pendingRemove.empty())
        {
            mDraftInputMap.RemoveAction(pendingRemove);
            CancelListening();
        }

        mPreviousRaw = raw;
    }
}
