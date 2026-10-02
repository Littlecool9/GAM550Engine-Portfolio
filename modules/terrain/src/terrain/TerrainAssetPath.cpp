/**
 * @file TerrainAssetPath.cpp
 * @author Jackey Cen
 * @brief Maps a terrain recipe path to its asset id, and a game to its releases root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainAssetPath.h"

#include <format>
#include <vector>

namespace mir
{
    namespace
    {
        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool EqualsIgnoringCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (Lower(a[i]) != Lower(b[i]))
                    return false;
            return true;
        }

        bool EndsWithIgnoringCase(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && EqualsIgnoringCase(text.substr(text.size() - suffix.size()), suffix);
        }
    }

    TerrainAssetPath ResolveTerrainAssetPath(std::string_view idRoot, std::string_view sourcePath)
    {
        TerrainAssetPath result;
        if (const AssetPathResult root = NormalizeAssetPath(idRoot); !root.Ok())
        {
            result.error = TerrainAssetPathError::IdRoot;
            result.pathError = root.error;
            return result;
        }
        AssetPathResult logical = MakeLogicalAssetPath(idRoot, sourcePath);
        if (!logical.Ok())
        {
            result.error = TerrainAssetPathError::SourcePath;
            result.pathError = logical.error;
            return result;
        }
        // The cooker skips any case; only the exact spelling names a terrain asset.
        const std::string_view name = std::string_view(logical.path).substr(logical.path.rfind('/') + 1);
        if (name.size() <= kTerrainRecipeSuffix.size() || !name.ends_with(kTerrainRecipeSuffix))
        {
            result.error = TerrainAssetPathError::NotARecipe;
            result.logicalPath = std::move(logical.path);
            return result;
        }
        result.asset = MakeAssetId(logical.path);
        result.logicalPath = std::move(logical.path);
        return result;
    }

    std::string DescribeTerrainAssetPathError(const TerrainAssetPath &path, std::string_view idRoot,
                                              std::string_view sourcePath)
    {
        switch (path.error)
        {
        case TerrainAssetPathError::None:
            return {};
        case TerrainAssetPathError::IdRoot:
            return std::format("id root \"{}\": {}", idRoot, Describe(path.pathError));
        case TerrainAssetPathError::SourcePath:
            return std::format("recipe path \"{}\": {}", sourcePath, Describe(path.pathError));
        case TerrainAssetPathError::NotARecipe:
            return std::format("recipe path \"{}\" does not end with \"{}\" (spelled exactly so: asset ids keep case)",
                               sourcePath, kTerrainRecipeSuffix);
        }
        return "unknown terrain asset path error";
    }

    std::filesystem::path TerrainReleasesRoot(const std::filesystem::path &gameDir)
    {
        return gameDir / "terrain" / "releases";
    }

    bool IsTerrainFileExcludedFromCook(std::string_view path)
    {
        if (EndsWithIgnoringCase(path, kTerrainRecipeSuffix) || EndsWithIgnoringCase(path, ".tindex") ||
            EndsWithIgnoringCase(path, ".tpack"))
            return true;

        std::vector<std::string_view> segments;
        size_t begin = 0;
        while (begin <= path.size())
        {
            size_t end = path.find_first_of("/\\", begin);
            if (end == std::string_view::npos)
                end = path.size();
            segments.push_back(path.substr(begin, end - begin));
            begin = end + 1;
        }
        for (size_t i = 0; i + 2 < segments.size(); ++i)
            if (EqualsIgnoringCase(segments[i], "terrain") && EqualsIgnoringCase(segments[i + 1], "releases"))
                return true;
        return false;
    }
}
