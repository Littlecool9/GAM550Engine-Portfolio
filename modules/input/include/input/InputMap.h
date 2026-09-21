/**
 * @file InputMap.h
 * @author Rahul Nair
 * @brief The authored action -> binding table (Project Settings' input
 *        section). A World resource - InputSystem reads it every frame, so
 *        editing it live (e.g. from a future rebind UI) takes effect at once.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputTypes.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace mir
{
    class InputMap
    {
    public:
        InputAction &AddAction(std::string name, InputActionType type);
        bool RemoveAction(std::string_view name);

        InputAction *Find(std::string_view name);
        const InputAction *Find(std::string_view name) const;

        const std::vector<InputAction> &Actions() const { return mActions; }

        std::vector<InputAction> &MutableActions() { return mActions; }

        // nullopt only when the file cannot be read as a JSON object at all.
        static std::optional<InputMap> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;

    private:
        std::vector<InputAction> mActions;
    };
}
