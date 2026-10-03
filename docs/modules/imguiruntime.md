# imguiruntime

The ImGui context's lifetime: the context itself, the fonts, the GLFW and Vulkan
backends, and the `NewFrame`/`Render` bracket around each frame. It draws no UI
and holds no theme, docking or layout; those belong to whoever draws.

`mir::imguiruntime` · depends `mir::renderer`, `mir::window` (public); `mir::core`, `imgui`, `iconfontcppheaders` (private) · API `modules/imguiruntime/include/imguiruntime/` · maintainer Quy Dong Ta · reviewed 2026-09-26

## Model

One `ImGuiRuntime` per process, held by `App`, sitting between the renderer and
every UI module. The UI modules (`editor` today, `gameui` next) draw into the
context it owns, and none of them owns any part of that context's lifetime.

```cpp
mImGui.Initialize(*mWindow, mRenderer);                 // after the renderer
mEditor.Initialize(mImGui, *mWindow, projectSettingsPath, mWorld);

// each frame
mImGui.BeginFrame();                                    // the NewFrame trio
mEditor.DrawLayout(...);                                // every UI module draws here
mRenderer.SetImGuiDrawData(mImGui.EndFrame());          // ImGui::Render, GetDrawData

// teardown, reverse order
mEditor.Shutdown();
mImGui.Shutdown(mRenderer);                             // before mRenderer.Shutdown()
```

It is a module of its own because neither obvious owner is allowed to hold it.
`ImGui_ImplGlfw_InitForVulkan` needs a `GLFWwindow *`, and `renderer` never sees
`window`. `App.cpp` makes no ImGui call. `editor` was the owner until 2026-09-26,
which made an editor-free build impossible, since a game UI had nowhere else to
get a context from.

## API

| Type | Header | Role |
|---|---|---|
| `ImGuiRuntime` | `imguiruntime/ImGuiRuntime.h` | `Initialize`/`Shutdown` the context and both backends; `BeginFrame`/`EndFrame` bracket a frame |

`VulkanRenderer::InitImGuiVulkanBackend` and `ShutdownImGuiVulkanBackend` stay in
`renderer`, because they need the device, queue and swapchain format. This module
is their only caller.

## Rules

**This module calls `NewFrame` and `Render`, and nobody else does.** Every UI
module draws between `BeginFrame` and `EndFrame`. A second `NewFrame` in a frame
asserts inside ImGui, and a UI module that brackets its own frame would lock every
other UI module out of that frame.

**It sets no `ConfigFlags` and leaves `IniFilename` null.** Docking and
`imgui.ini` are editor choices, so `editor` turns them on in its own `Initialize`.
A build without `editor` persists nothing and docks nothing. ImGui reads the ini
file lazily on the first `NewFrame`, which is why a UI module can set the path
after this module's `Initialize` returns.

**Whoever points `IniFilename` at a string owns detaching it.** `DestroyContext`
saves through that pointer, so a UI module that sets it must save and null it in
its own `Shutdown`, which runs first. `Editor::Shutdown` does.

**Order in `App` is load-bearing.** `App.h` declares `mImGui` after `mRenderer`
and before `mEditor`: the runtime initialises after the renderer and before any
UI module, and shuts down after every UI module and before the renderer, so the
Vulkan backend is gone before the device is and the GLFW backend before the
window.

**The Vulkan backend frees `gameui`'s textures.** `gameui` registers each UI
texture with the context as an ImGui user texture, and
`ImGui_ImplVulkan_Shutdown` frees every registered texture's image and
descriptor, as it does the font atlas's. So `App`'s `UIContext` outlives
`Shutdown` and goes with `App`, and the order above still holds for everything
it draws. See [gameui.md](gameui.md).

**`~ImGuiRuntime()` is a no-op if `Shutdown()` was skipped.** It leaks the context
rather than reach into a `VulkanRenderer&` it was never given.

**The fonts are shared.** Inter at 16px with Font Awesome 6 merged into the same
atlas. Every UI module gets the icon glyphs; DPI scaling is still each module's
own style decision (`EditorTheme::ScaleForDpi` in the editor's case).

## State

*As of 2026-09-27.* Owns the full lifetime and the frame bracket. `App` calls it
directly; `editor` and `gameui` are its UI clients.

No tests. It needs a device and a display. The `smoke.*` tests cover startup,
every frame and shutdown, and `smoke.switch` covers the restart that reloads
`imgui.ini`.

## Backlog

1. ~~**`gameui`**, the second client~~, landed 2026-09-27; it draws into the
   background list between the same `BeginFrame` and `EndFrame`. See
   [gameui.md](gameui.md).
2. **Input capture.** `App` still asks `Editor::WantsCaptureKeyboard`/
   `WantsCaptureMouse`, which read the shared `ImGuiIO`. Once `gameui` also
   draws, that question is about the context rather than the editor, and belongs
   here.
3. **Font choice per build.** The icon font is loaded unconditionally; a game
   that wants its own fonts has no hook yet.

## Changed

*2026-10-02* — **The editor-free build exists.** `MJOLNIR_WITH_EDITOR` OFF
builds without `editor`, and this module is then the context's only owner and
`gameui` its only client. Nothing here changed for it: no `ConfigFlags`, no
`imgui.ini`, and the shared fonts, as the rules above already required. See
[build.md](../build.md#a-build-without-the-editor).

*2026-09-26* — **Created from `Editor::Initialize`/`Shutdown`/`BeginFrame`/`EndFrame`.**
Was: `editor` created the context, loaded the fonts, initialised both backends and
bracketed the frame. Now: this module does, so `editor` and a future `gameui` both
draw into a context neither owns.
