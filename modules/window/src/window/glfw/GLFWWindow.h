/**
 * @file GLFWWindow.h
 * @author Sumin Park
 * @brief GLFW window for desktop platforms
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "window/Window.h"

#include <span>
#include <string>
#include <vector>

struct GLFWwindow;

namespace mir
{
    class GLFWWindow final : public Window
    {
    public:
        explicit GLFWWindow(const WindowDesc &desc);
        ~GLFWWindow() override;

        void PollEvents() override;
        bool ShouldClose() const override;

        uint32_t Width() const override { return mWidth; }
        uint32_t Height() const override { return mHeight; }

        NativeWindowHandle NativeWindow() const override;
        void *NativeHandleForImGui() const override { return mHandle; }
        float ContentScale() const override;

        bool HasCustomTitleBar() const override { return mHasCustomTitleBar; }
        const char *Title() const override { return mTitle.c_str(); }
        void SetTitleBarInteractiveRects(std::span<const PixelRect> rects) override
        {
            mTitleBarInteractiveRects.assign(rects.begin(), rects.end());
        }

        void Minimize() override;
        void ToggleMaximize() override;
        bool IsMaximized() const override;
        void RequestClose() override;
        void Show() override;

        const RawInputSnapshot &RawInput() const override { return mRawInput; }

        // Read by the Win32-only title bar WndProc hook to answer
        // WM_NCHITTEST; meaningless elsewhere.
        std::span<const PixelRect> TitleBarInteractiveRects() const { return mTitleBarInteractiveRects; }

    private:
        static void OnFramebufferSize(GLFWwindow *handle, int width, int height);

        static void OnScroll(GLFWwindow *handle, double xoffset, double yoffset);

        // Win32 only: installs/removes the WndProc subclass answering
        // WM_NCCALCSIZE / WM_NCHITTEST / WM_NCACTIVATE for the custom title
        // bar. No-op on other platforms (GLFWLinux.cpp) - no Win32 types
        // appear in this shared header.
        void InstallCustomTitleBar();
        void UninstallCustomTitleBar();

        GLFWwindow *mHandle = nullptr;
        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
        std::string mTitle;
        bool mHasCustomTitleBar = false;
        std::vector<PixelRect> mTitleBarInteractiveRects;
        RawInputSnapshot mRawInput;
    };
}
