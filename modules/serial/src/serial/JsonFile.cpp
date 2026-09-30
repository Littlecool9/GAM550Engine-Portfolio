/**
 * @file JsonFile.cpp
 * @author Sumin Park
 * @brief Class for JSON control. Interpretates bytes from core/fs.FileIO.h
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "serial/JsonFile.h"

#include "core/fs/FileIO.h"
#include "core/log/Log.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    std::optional<json> ParseJson(std::string_view text, std::string_view context)
    {
        try
        {
            return json::parse(text);
        }
        catch (const json::exception &e)
        {
            MIR_LOG_ERROR("ParseJson: '{}' is not valid JSON ({})", context, e.what());
            return std::nullopt;
        }
    }

    std::optional<json> ReadJsonFile(const std::filesystem::path &path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return std::nullopt;

        const std::optional<std::string> text = ReadText(path);
        if (!text)
            return std::nullopt; // ReadText already logged why

        return ParseJson(*text, path.string());
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
            const std::optional<std::string> text = ReadText(path);
            if (!text)
            {
                MIR_LOG_ERROR("UpdateJsonFile: cannot read '{}'", path.string());
                return false;
            }

            // A blank file holds no sections worth protecting, so it starts a fresh
            // object. Refusing it would leave the caller unable to ever save.
            const bool blank = std::all_of(text->begin(), text->end(),
                [](unsigned char c) { return std::isspace(c) != 0; });
            if (!blank)
            {
                const std::optional<json> existing = ParseJson(*text, path.string());
                if (!existing || !existing->is_object())
                {
                    MIR_LOG_ERROR("UpdateJsonFile: refusing to overwrite '{}', not a readable JSON object",
                        path.string());
                    return false;
                }

                root = std::move(*existing);
            }
        }

        edit(root);
        return WriteJsonFile(path, root, indent);
    }
}
