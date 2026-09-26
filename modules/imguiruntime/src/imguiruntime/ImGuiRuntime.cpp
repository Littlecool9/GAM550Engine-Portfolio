/**
 * @file ImGuiRuntime.cpp
 * @author Quy Dong Ta
 * @brief Owns the ImGui context's lifetime: context, fonts, both backends,
 *        and the per-frame NewFrame/Render bracket that every UI draws within.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <imguiruntime/ImGuiRuntime.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <string>

namespace mir
{

    ImGuiRuntime::~ImGuiRuntime() {}

    bool ImGuiRuntime::Initialize(Window &window, VulkanRenderer &renderer)
    {
        ImGui::CreateContext();

        ImGuiIO &io = ImGui::GetIO();

        io.IniFilename = nullptr;

        ImFontConfig fontConfig;
        fontConfig.OversampleH = 3;
        const std::string fontPath = FontPath("Inter.ttf").string();

        if (io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig) == nullptr)
        {
            MIR_LOG_ERROR("Failed to load font: {}", fontPath);
            io.Fonts->AddFontDefault();
        }

        static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = 16.0f;
        const std::string iconFontPath = FontPath(FONT_ICON_FILE_NAME_FAS).string();
        
        if (io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), 16.0f, &iconConfig, iconRanges) == nullptr)
            MIR_LOG_ERROR("Failed to load icon font: {}", iconFontPath);

        void *nativeHandle = window.NativeHandleForImGui();
        if (nativeHandle == nullptr)
        {
            MIR_LOG_ERROR("Window backend has no native handle for ImGui");
            ImGui::DestroyContext();
            return false;
        }

        if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow *>(nativeHandle), true))
        {
            MIR_LOG_ERROR("ImGui_ImplGlfw_InitForVulkan failed");
            ImGui::DestroyContext();
            return false;
        }

        if (!renderer.InitImGuiVulkanBackend())
        {
            MIR_LOG_ERROR("ImGui Vulkan backend initialization failed");
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        mInitialized = true;
        return true;
    }

    void ImGuiRuntime::Shutdown(VulkanRenderer &renderer)
    {
        if (!mInitialized)
            return;

        renderer.ShutdownImGuiVulkanBackend();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        mInitialized = false;
    }

    void ImGuiRuntime::BeginFrame()
    {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    ImDrawData *ImGuiRuntime::EndFrame()
    {
        ImGui::Render();
        return ImGui::GetDrawData();
    }
} // namespace mir
