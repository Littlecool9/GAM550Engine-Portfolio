/**
 * @file Paths.h
 * @author Sumin Park
 * @brief Executable-relative asset paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <string_view>

namespace mir
{
    // Full path of the running executable; empty if unknown
    const std::filesystem::path &ExecutablePath();

    const std::filesystem::path &ExecutableDir();

    std::filesystem::path ShaderPath(std::string_view name);
    std::filesystem::path FontPath(std::string_view name);
    std::filesystem::path BrandingPath(std::string_view name);

    std::filesystem::path CookedAssetsDir();
}
