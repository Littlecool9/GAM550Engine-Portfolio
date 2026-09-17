/**
 * @file RenderSettings.h
 * @author Rahul Nair
 * @brief Project Settings' "rendering" section. A World resource, loaded and
 *        saved the same way InputMap is (see input/InputMap.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>

namespace mir
{
    struct RenderSettings
    {
        bool rayTracing = false;

        static RenderSettings LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };
}
