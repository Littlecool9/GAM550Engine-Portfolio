/**
 * @file JsonFile.h
 * @author Sumin Park
 * @brief Class for JSON control. Interpretates bytes from core/fs.FileIo.h
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>

namespace mir
{
    std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path &path);

    // Dump and write atomically
    bool WriteJsonFile(const std::filesystem::path &path, const nlohmann::json &root, int indent = 2);

    // Read @p path, hand the object to @p edit, write the result and report success/fail.
    bool UpdateJsonFile(const std::filesystem::path &path,
        const std::function<void(nlohmann::json &)> &edit,
        int indent = 2);
}
