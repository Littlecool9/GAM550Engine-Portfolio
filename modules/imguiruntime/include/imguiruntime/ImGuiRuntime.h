/**
 * @file ImGuiRuntime.h
 * @author Quy Dong Ta
 * @brief Owns the ImGui context's lifetime: context, fonts, both backends,
 *        and the per-frame NewFrame/Render bracket that every UI draws within.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

struct ImDrawData;

namespace mir
{
    class ImGuiRuntime
    {
    public:
        ImGuiRuntime() = default;
        ~ImGuiRuntime();

        ImGuiRuntime(const ImGuiRuntime &) = delete;
        ImGuiRuntime &operator=(const ImGuiRuntime &) = delete;
        ImGuiRuntime(ImGuiRuntime &&) = delete;
        ImGuiRuntime &operator=(ImGuiRuntime &&) = delete;

        //call after the renderer but before any UI module's Initialize
        bool Initialize(Window &window, VulkanRenderer &renderer);

        //after every UI module's Shutdown but before the renderer's
        void Shutdown(VulkanRenderer &renderer);

        //ui modules' draws should be between this and endframe
        void BeginFrame();

        ImDrawData *EndFrame();

        bool IsInitialized() const { return mInitialized; }

    private:
        bool mInitialized = false;
    };
} // namespace mir
