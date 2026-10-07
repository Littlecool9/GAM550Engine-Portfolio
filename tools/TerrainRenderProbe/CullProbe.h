#pragma once
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>
#include <nlohmann/json.hpp>
#include <filesystem>

nlohmann::json RunCullProbe(mir::VulkanRenderer &renderer,mir::Window &window,
    const std::filesystem::path &out,uint32_t width,uint32_t height);
