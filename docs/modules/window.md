# window

Gets an OS window on screen, pumps its events, reports whether it should close,
and hands out a native handle and size. It does not do input, and it does not
touch a graphics API.

`mir::window` · depends `mir::core` (public); `glfw`, `windowscodecs`, `ole32` (private, desktop only) · API `modules/window/include/window/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

One public header declares an interface, and a backend is chosen at build time
rather than at runtime.

`Window.h` declares `WindowDesc`, the abstract `Window`, and the static factory
`Window::Create`. Nothing in the public part of the module defines `Create` — the
definition lives in whichever backend `CMakeLists.txt` compiled. A caller
therefore holds a `unique_ptr<Window>` and never names a backend type.

`Window` adds two methods of its own, `PollEvents` and `ShouldClose`. The other
three come from `ISurfaceProvider`, which is declared in `core`. That split is
the point of the design: the renderer takes the base and never sees the event
pump.

GLFW is created with `GLFW_NO_API`. It appears in no public header and in no
translation unit outside this module.

The GLFW backend is split in two. `glfw/GLFWWindow.{h,cpp}` holds everything
portable — creation, the event pump, the close flag, framebuffer size tracking,
and the init refcount. `glfw/GLFWWin32.cpp` and `glfw/GLFWLinux.cpp` hold one
function each, because native handle extraction is the only part that needs
`glfw3native.h` and its per-platform `GLFW_EXPOSE_NATIVE_*` defines.

The selection happens in the module's CMakeLists. `ENGINE_FAMILY_DESKTOP` guards
the whole backend, and `ENGINE_PLATFORM` picks the native file within it.

```cpp
WindowDesc desc{};
desc.mWidth = 1280;
desc.mHeight = 720;
desc.mTitle = "MjolnirEngine";

std::unique_ptr<Window> window = Window::Create(desc);

while (!window->ShouldClose())
{
    window->PollEvents();
    // draw, using window.get() as an ISurfaceProvider
}
```

## API

| Type | Header | Role |
|---|---|---|
| `WindowDesc` | `window/Window.h` | Size, title, resizable |
| `Window` | `window/Window.h` | The interface, plus `Create` |
| `SplashScreen` | `window/Splash.h` | Native startup splash, shown before the window exists |
| `GLFWWindow` | `src/window/glfw/GLFWWindow.h` | Desktop implementation, not public |
| `ISurfaceProvider` | `core/platform/Surface.h` | Base of `Window` — `NativeWindow`, `Width`, `Height` |
| `NativeWindowHandle` | `core/platform/Surface.h` | `{backend, display, window}` |

`NativeWindowHandle::display` is null on Win32 and Cocoa. Elsewhere it is the X11
or Wayland display.

## Rules

**Only the desktop family has a backend.** On mobile or web, nothing defines
`Window::Create` and the link fails.

An unrecognised desktop platform is caught earlier than that. macOS passes the
family check but matches neither `ENGINE_PLATFORM` branch, so the build stops at
configure time with `No native window backend for macOS`.

**Main thread only.** GLFW requires `glfwInit`, window creation and
`glfwPollEvents` to happen on the main thread, and the init refcount is a plain
`int` with no synchronisation. Creating or destroying a window off-thread is a
data race on top of a GLFW violation.

**`PollEvents` has to be called, and often.** It is what drives the framebuffer
resize callback and the close flag. Skip it and `ShouldClose` never becomes true
and the reported size goes stale.

Size is a cached value updated only by that callback, not a query.

**The window outlives its renderer.** The renderer builds a `VkSurfaceKHR` from
the native handle, so destroying the window first leaves that surface pointing at
a dead window. `App::Shutdown` encodes the order.

**`Width` and `Height` are framebuffer pixels, not screen coordinates.** They are
legitimately `0x0` while the window is minimised, and the window does not filter
that out. The consumer decides whether to skip the frame.

**`Window` is neither copyable nor movable.** Own it through a `unique_ptr`. The
backend holds a raw `GLFWwindow*` that it destroys, and GLFW holds a user pointer
back to the instance.

That user pointer is how the static framebuffer callback recovers the owning
`GLFWWindow`. Any further callback added here follows the same route.

**Failure aborts rather than returning.** `glfwInit` and `glfwCreateWindow` are
wrapped in `MIR_CHECK`, so `Window::Create` never returns null on the desktop
backend.

This departs from the convention that init paths return `bool`, and it leaves
`App::Initialize`'s `if (!mWindow)` branch unreachable. See Backlog.

**The GLFW error callback is installed before `glfwInit`**, so GLFW's own
diagnostics go through `MIR_LOG_ERROR` rather than being lost.

`GLFWLinux.cpp` picks between Wayland and X11 at runtime with `glfwGetPlatform()`,
even though the file itself was selected at build time.

The X11 window id is an `unsigned long` rather than a pointer, so it is cast
through `uintptr_t` to fit `NativeWindowHandle::window`. Anything reading that
field on Xlib has to cast back the same way.

## State

*As of 2026-09-06.* Creation, the event pump, the close flag, framebuffer size,
content scale, the native handle and a native splash screen all work. That is the
whole module.

No tests. Everything here needs a real window server, since `glfwInit` fails
headless, and what remains after removing that is a handful of forwarding calls.
The module is covered in practice by the engine failing to start.

## Backlog

1. **Input.** No keyboard, mouse or gamepad, and no event type at all — callbacks
   are not surfaced to the caller. Undecided where it belongs: more virtuals on
   `Window`, a separate `input` module fed by the backend, or an event queue
   drained by a system.
2. **Resize and close notification.** The size updates silently and nothing is
   told.
3. Fullscreen, monitor selection, cursor control, runtime title changes, and
   programmatic close.
4. macOS has no native translation unit, so a macOS configure fails. Linux
   sources exist but there is no Linux preset, so that path is unbuilt.
5. Undecided: `MIR_CHECK` versus returning `nullptr`. `App` already has a branch
   the current backend can never take, so the two disagree and one of them should
   change.
6. Undecided: whether multiple windows are a goal. The GLFW init refcount
   supports it, so the first window to die does not take the library down with
   it, but nothing else in the engine does.
