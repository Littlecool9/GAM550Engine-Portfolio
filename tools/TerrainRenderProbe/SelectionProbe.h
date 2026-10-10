/**
 * @file SelectionProbe.h
 * @author Jackey Cen
 * @brief Captures per-viewport tile and Region selections while they stream, from several views and two cameras.
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

nlohmann::json RunSelectionProbe(mir::VulkanRenderer &renderer, mir::Window &window, const std::filesystem::path &out,
                                 uint32_t width, uint32_t height);
