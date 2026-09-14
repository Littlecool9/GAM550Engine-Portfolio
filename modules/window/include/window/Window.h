/**
 * @file Window.h
 * @author Sumin Park
 * @brief The interface representing the window
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/platform/Surface.h>
#include <window/InputSnapshot.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace mir
{
    void EnsureDpiAware();

    struct WindowDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;
        const char *mTitle = "MjolnirEngine";
        bool mResizable = true;
        bool mMaximized = false;
        bool mCustomTitleBar = false;

        bool mStartHidden = false;
    };

    class Window : public ISurfaceProvider
    {
    public:
        Window(const Window &) = delete;
        Window &operator=(const Window &) = delete;
        Window(Window &&) = delete;
        Window &operator=(Window &&) = delete;

        virtual void PollEvents() = 0;
        virtual bool ShouldClose() const = 0;

        virtual void *NativeHandleForImGui() const { return nullptr; }

        // 1.0 at 96 DPI, scaling up with the monitor's content scale.
        virtual float ContentScale() const { return 1.0f; }

        // Shared source of truth between the UI layer's title bar strip and
        // the platform hit-test math - both must agree exactly on where the
        // strip ends.
        static constexpr float kTitleBarHeightDip = 32.0f;

        virtual bool HasCustomTitleBar() const { return false; }
        virtual const char *Title() const { return ""; }

        // Replaces the title given at creation, e.g. when another game opens.
        virtual void SetTitle(const std::string &title) {}

        virtual void SetTitleBarInteractiveRects(std::span<const PixelRect> rects) {}

        virtual void Minimize() {}
        virtual void ToggleMaximize() {}
        virtual bool IsMaximized() const { return false; }
        virtual void RequestClose() {}

        virtual void Show() {}

        virtual const RawInputSnapshot &RawInput() const
        {
            static const RawInputSnapshot kEmpty{};
            return kEmpty;
        }

        static std::unique_ptr<Window> Create(const WindowDesc &desc);

    protected:
        Window() = default;
    };
};
