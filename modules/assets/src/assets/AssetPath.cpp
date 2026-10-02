/**
 * @file AssetPath.cpp
 * @author Jackey Cen
 * @brief Logical asset path normalization shared by the engine and asset tools.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "assets/AssetPath.h"

#include <cctype>
#include <vector>

namespace mir
{
    std::string_view Describe(AssetPathError error)
    {
        switch (error)
        {
        case AssetPathError::None:
            return "ok";
        case AssetPathError::Empty:
            return "path is empty";
        case AssetPathError::Absolute:
            return "path is absolute; asset paths are relative to the asset root";
        case AssetPathError::InvalidCharacter:
            return "path contains ':' or a NUL character";
        case AssetPathError::EscapesRoot:
            return "path climbs out of the asset root with '..'";
        }
        return "unknown asset path error";
    }

    AssetPathResult NormalizeAssetPath(std::string_view path)
    {
        if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
            return {{}, AssetPathError::Absolute};
        if (path.find(':') != std::string_view::npos || path.find('\0') != std::string_view::npos)
            return {{}, AssetPathError::InvalidCharacter};
        if (!path.empty() && (path.front() == '/' || path.front() == '\\'))
            return {{}, AssetPathError::Absolute};

        std::vector<std::string_view> segments;
        size_t begin = 0;
        while (begin <= path.size())
        {
            size_t end = path.find_first_of("/\\", begin);
            if (end == std::string_view::npos)
                end = path.size();
            const std::string_view segment = path.substr(begin, end - begin);
            if (segment == "..")
            {
                if (segments.empty())
                    return {{}, AssetPathError::EscapesRoot};
                segments.pop_back();
            }
            else if (!segment.empty() && segment != ".")
            {
                segments.push_back(segment);
            }
            begin = end + 1;
        }

        if (segments.empty())
            return {{}, AssetPathError::Empty};

        AssetPathResult result;
        for (const std::string_view segment : segments)
        {
            if (!result.path.empty())
                result.path += '/';
            result.path += segment;
        }
        return result;
    }

    AssetPathResult MakeLogicalAssetPath(std::string_view idRoot, std::string_view relative)
    {
        AssetPathResult root = NormalizeAssetPath(idRoot);
        if (!root.Ok())
            return root;

        // Normalized on its own first, so "../x" cannot climb into a sibling root.
        AssetPathResult rest = NormalizeAssetPath(relative);
        if (!rest.Ok())
            return rest;

        root.path += '/';
        root.path += rest.path;
        return root;
    }
}
