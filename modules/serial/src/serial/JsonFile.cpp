/**
 * @file JsonFile.cpp
 * @author Sumin Park
 * @brief Class for JSON control. Interpretates bytes from core/fs.FileIo.h
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "serial/JsonFile.h"

#include "core/fs/FileIo.h"
#include "core/log/Log.h"

#include <string>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    std::optional<json> ReadJsonFile(const std::filesystem::path &path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return std::nullopt;

        const std::optional<std::string> text = ReadText(path);
        if (!text)
            return std::nullopt; // ReadText already logged why

        try
        {
            return json::parse(*text);
        }
        catch (const json::parse_error &e)
        {
            MIR_LOG_ERROR("ReadJsonFile: '{}' is not valid JSON ({})", path.string(), e.what());
            return std::nullopt;
        }
    }

    bool WriteJsonFile(const std::filesystem::path &path, const json &root, int indent)
    {
        std::string dumped;
        try
        {
            dumped = root.dump(indent);
        }
        catch (const json::type_error &e)
        {
            // dump() throws on a string field that is not valid UTF-8, which a
            // fixed-size char buffer filled from user input can easily be.
            MIR_LOG_ERROR("WriteJsonFile: cannot serialize for '{}' ({})", path.string(), e.what());
            return false;
        }

        return WriteTextAtomic(path, dumped);
    }

    bool UpdateJsonFile(const std::filesystem::path &path,
        const std::function<void(json &)> &edit,
        int indent)
    {
        json root = json::object();

        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            const std::optional<json> existing = ReadJsonFile(path);
            if (!existing)
            {
                MIR_LOG_ERROR("UpdateJsonFile: refusing to overwrite unreadable '{}'", path.string());
                return false;
            }

            if (!existing->is_object())
            {
                MIR_LOG_ERROR("UpdateJsonFile: '{}' is JSON but not an object, refusing to overwrite",
                    path.string());
                return false;
            }

            root = std::move(*existing);
        }

        edit(root);
        return WriteJsonFile(path, root, indent);
    }
}
