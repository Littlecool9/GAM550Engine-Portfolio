/**
 * @file GLFWWin32.cpp
 * @author Sumin Park
 * @brief Win32 specifics for GLFW
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // GetDpiForWindow / GetSystemMetricsForDpi (Windows 10 1607+)
#endif

#include "GLFWWindow.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windowsx.h>

#include <mutex>

namespace mir
{
    void EnsureDpiAware()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        });
    }

    NativeWindowHandle GLFWWindow::NativeWindow() const
    {
        return {WindowBackend::Win32, nullptr, glfwGetWin32Window(mHandle)};
    }

    namespace
    {
        constexpr wchar_t kTitleBarPropName[] = L"MirTitleBarHook";

        struct TitleBarHookState
        {
            GLFWWindow *window = nullptr;
            WNDPROC prevWndProc = nullptr;
        };

        int ResizeBorderThicknessPx(HWND hwnd)
        {
            const UINT dpi = ::GetDpiForWindow(hwnd);
            return ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        }

        LRESULT CALLBACK TitleBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            auto *state = static_cast<TitleBarHookState *>(::GetPropW(hwnd, kTitleBarPropName));
            if (state == nullptr)
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);

            switch (msg)
            {
            case WM_NCCALCSIZE:
            {
                // Claim the whole window as client area either way. When
                // maximized, Windows still needs the invisible resize-frame
                // padding subtracted back in, or the window overhangs each
                // monitor edge by that amount. Fullscreen covers the monitor
                // exactly, so it takes no padding.
                if (wParam == TRUE && ::IsZoomed(hwnd) && !state->window->IsFullscreen())
                {
                    auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
                    const int frame = ResizeBorderThicknessPx(hwnd);
                    params->rgrc[0].left += frame;
                    params->rgrc[0].top += frame;
                    params->rgrc[0].right -= frame;
                    params->rgrc[0].bottom -= frame;
                }
                return 0;
            }

            case WM_NCACTIVATE:
                // lParam = -1 tells DefWindowProc not to repaint the (now
                // invisible) non-client area - avoids an activate/deactivate
                // flicker.
                return ::DefWindowProcW(hwnd, msg, wParam, -1);

            case WM_NCHITTEST:
            {

                if (state->window->IsFullscreen())
                    return HTCLIENT;

                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ScreenToClient(hwnd, &pt);
                RECT client{};
                ::GetClientRect(hwnd, &client);

                if (!::IsZoomed(hwnd))
                {
                    const int border = ResizeBorderThicknessPx(hwnd);
                    const bool left = pt.x < border;
                    const bool right = pt.x >= client.right - border;
                    const bool top = pt.y < border;
                    const bool bottom = pt.y >= client.bottom - border;

                    if (top && left) return HTTOPLEFT;
                    if (top && right) return HTTOPRIGHT;
                    if (bottom && left) return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                }

                const UINT dpi = ::GetDpiForWindow(hwnd);
                const int titleBarBottom = static_cast<int>(Window::kTitleBarHeightDip * static_cast<float>(dpi) / 96.0f);
                if (pt.y < titleBarBottom)
                {
                    for (const PixelRect &r : state->window->TitleBarInteractiveRects())
                    {
                        if (pt.x >= r.x && pt.x < r.x + static_cast<int>(r.width) &&
                            pt.y >= r.y && pt.y < r.y + static_cast<int>(r.height))
                            return HTCLIENT;
                    }
                    return HTCAPTION;
                }
                return HTCLIENT;
            }

            default:
                break;
            }

            return ::CallWindowProcW(state->prevWndProc, hwnd, msg, wParam, lParam);
        }
    }

    void GLFWWindow::InstallCustomTitleBar()
    {
        HWND hwnd = static_cast<HWND>(glfwGetWin32Window(mHandle));

        auto *state = new TitleBarHookState{
            this, reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(hwnd, GWLP_WNDPROC))};
        ::SetPropW(hwnd, kTitleBarPropName, state);
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TitleBarWndProc));

        // Windows only re-runs WM_NCCALCSIZE on the next resize/move without
        // this - the native chrome would otherwise hang around until then.
        ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void GLFWWindow::UninstallCustomTitleBar()
    {
        HWND hwnd = static_cast<HWND>(glfwGetWin32Window(mHandle));
        if (auto *state = static_cast<TitleBarHookState *>(::GetPropW(hwnd, kTitleBarPropName)))
        {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->prevWndProc));
            ::RemovePropW(hwnd, kTitleBarPropName);
            delete state;
        }
    }

    void GLFWWindow::PlaceCustomTitleBarWindow(int x, int y, int width, int height)
    {
        HWND hwnd = static_cast<HWND>(glfwGetWin32Window(mHandle));
        ::SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
