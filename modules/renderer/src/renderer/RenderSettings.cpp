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

#include <assets/AssetFileIo.h>

#include <nlohmann/json.hpp>

#include <string_view>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    RenderSettings RenderSettings::LoadFile(const std::filesystem::path &path)
    {
        RenderSettings settings;

        const std::optional<std::vector<std::byte>> bytes = ReadFileBytes(path);
        if (!bytes)
            return settings;

        json root;
        try
        {
            root = json::parse(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
        }
        catch (const json::parse_error &)
        {
            return settings;
        }

        if (!root.is_object())
            return settings;

        const json &renderingRoot = root.value("rendering", json::object());
        settings.rayTracing = renderingRoot.value("rayTracing", false);
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
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

        root["rendering"] = json{{"rayTracing", rayTracing}};

        const std::string dumped = root.dump(2);
        const std::span<const std::byte> bytesOut(reinterpret_cast<const std::byte *>(dumped.data()), dumped.size());
        return WriteFileBytes(path, bytesOut);
    }
}
