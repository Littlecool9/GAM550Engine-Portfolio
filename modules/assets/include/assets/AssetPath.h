/**
 * @file AssetPath.h
 * @author Jackey Cen
 * @brief Logical asset path normalization shared by the engine and asset tools.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mir
{
    enum class AssetPathError : uint8_t
    {
        None,
        Empty,
        Absolute,
        InvalidCharacter,
        EscapesRoot,
    };

    struct AssetPathResult
    {
        std::string path;
        AssetPathError error = AssetPathError::None;

        bool Ok() const { return error == AssetPathError::None; }
    };

    std::string_view Describe(AssetPathError error);

    // Purely lexical, never touches the disk: '\' becomes '/', empty and "."
    // segments drop, ".." pops one segment and fails with EscapesRoot when
    // there is none. Case sensitive
    AssetPathResult NormalizeAssetPath(std::string_view path);

    // The string MakeAssetId hashes: normalized idRoot (e.g. "games/HelloWorld/assets")
    // + '/' + normalized relative. relative may not climb out of idRoot. Renaming
    // or moving the source changes the id; references must be updated explicitly.
    AssetPathResult MakeLogicalAssetPath(std::string_view idRoot, std::string_view relative);
}
