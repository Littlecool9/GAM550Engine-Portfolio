#include "renderer/RenderSettings.h"

#include <core/log/Log.h>

#include <nlohmann/json.hpp>

#include <fstream>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

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
    }

    RenderSettings RenderSettings::LoadFile(const std::filesystem::path &path)
    {
        const json root = ReadRoot(path);

        RenderSettings settings;
        const json &renderingRoot = root.value("rendering", json::object());
        settings.rayTracing = renderingRoot.value("rayTracing", false);
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
    {
        json root = ReadRoot(path);
        root["rendering"] = json{{"rayTracing", rayTracing}};

        std::ofstream out(path);
        if (!out)
        {
            MIR_LOG_ERROR("RenderSettings::SaveFile: could not open '{}' for writing", path.string());
            return false;
        }

        out << root.dump(2);
        if (!out)
        {
            MIR_LOG_ERROR("RenderSettings::SaveFile: write to '{}' failed", path.string());
            return false;
        }

        return true;
    }
}
