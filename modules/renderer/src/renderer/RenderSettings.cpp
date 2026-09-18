#include "renderer/RenderSettings.h"

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    RenderSettings RenderSettings::LoadFile(const std::filesystem::path &path)
    {
        RenderSettings settings;

        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return settings;

        const json &renderingRoot = root->value("rendering", json::object());
        settings.rayTracing = renderingRoot.value("rayTracing", false);
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
    {
        return UpdateJsonFile(path, [this](json &root)
            {
                root["rendering"] = json{ { "rayTracing", rayTracing } };
            });
    }
}
