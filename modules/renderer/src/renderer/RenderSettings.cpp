/**
 * @file RenderSettings.cpp
 * @author Rahul Nair
 * @brief Project Settings' "rendering" section. A World resource, loaded and
 *        saved the same way InputMap is (see input/InputMap.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/RenderSettings.h"

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    std::optional<RenderSettings> RenderSettings::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return std::nullopt;

        RenderSettings settings;

        // find() rather than value(): value() hands back the raw node, and a
        // "rendering" that is null or a scalar then throws type_error 306 on the
        // next read. Absent or unusable both mean defaults.
        const auto renderingIt = root->find("rendering");
        if (renderingIt == root->end() || !renderingIt->is_object())
            return settings;

        settings.rayTracing = renderingIt->value("rayTracing", false);
        settings.fog = renderingIt->value("fog", false);
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
    {
        return UpdateJsonFile(path, [this](json &root)
            {
                root["rendering"] = json{ { "rayTracing", rayTracing }, { "fog", fog } };
            });
    }
}
