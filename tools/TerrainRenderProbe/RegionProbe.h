/**
 * @file RegionProbe.h
 * @author Jackey Cen
 * @brief Captures Region and tile selections from high-altitude and low-approach views for the size comparison.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>

nlohmann::json RunRegionProbe(mir::VulkanRenderer &renderer, mir::Window &window, const std::filesystem::path &out,
                              uint32_t width, uint32_t height);
