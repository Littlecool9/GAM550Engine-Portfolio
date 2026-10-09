#pragma once
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>
#include <nlohmann/json.hpp>
#include <filesystem>

nlohmann::json RunFrameRegressionProbe(mir::VulkanRenderer &renderer, mir::Window &window,
    const std::filesystem::path &out);
