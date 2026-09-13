# editor

The ImGui context and backend lifecycle, and the Slate editor shell — dockspace,
default panel layout, Debug menu. It is the only place in the engine that calls
`ImGui::*` or `ImGui_Impl*`.

`mir::editor` · depends `mir::renderer`, `mir::window` (public); `mir::core`, `mir::editortheme`, `imgui`, `iconfontcppheaders` (private) · API `modules/editor/include/editor/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

`App` drives `Editor` through four calls a frame and reads one piece of state
back.

```cpp
mEditor.BeginFrame();                                   // the ImGui NewFrame trio

if (windowVisible)
    Act(mEditor.DrawLayout(mDesc.mEnableEditorLayout)); // New / Save / Load, or None

mRenderer.SetImGuiDrawData(mEditor.EndFrame());         // ImGui::Render, hand off draw data
mRenderSystem.SetViewportRect(0, mEditor.SceneViewportRect());
```

`DrawLayout` returns the `SceneMenuAction` the File menu picked this frame. The
editor decides what was asked for and `App` decides what it means, so this module
never touches `SceneIO` or the `World`.

`DrawLayout(true)` builds the dockspace once. It splits into left, right and
bottom with `DockBuilderSplitNode`, keeps the centre undocked through
`ImGuiDockNodeFlags_PassthruCentralNode`, docks `Hierarchy`, `Inspector` and
`Output` into the splits, draws the `Debug` menu bar, and optionally the Style
Editor.

Passing `false` skips all of that and resets `SceneViewportRect()` to zero
extent, which the renderer reads as "use the full swapchain". An ImGui context
still runs, so a caller can draw its own UI.

## API

```cpp
bool Initialize(Window &window, VulkanRenderer &renderer);
void Shutdown(VulkanRenderer &renderer);

void BeginFrame();
SceneMenuAction DrawLayout(bool enableLayout);   // None / New / Save / Load
ImDrawData *EndFrame();

VkRect2D SceneViewportRect() const;
bool IsInitialized() const;
```

`LogPanel`, in `src/editor/panels/LogPanel.cpp`, renders `core`'s `LogHistory`
into the `Output` dock. It is not public — panels are reached only through
`DrawLayout`.

## Rules

**`SceneViewportRect()` comes from `DockBuilderGetCentralNode`, never from a
named window.** The central node is the undocked centre where the swapchain shows
through, so it is the real viewport even though it has no window and no name.

Reading a window's content region instead is the bug this rule exists to prevent.
See Changed.

Anyone adding a genuine named viewport window has to update `DrawLayout`'s
source. There is no compiler or runtime check for it.

**The rect is only valid within or after `DrawLayout(true)` in the same frame.**

**Destruction order is load-bearing.** `App` declares `Editor` right after
`mRenderer` and before `mWorld`, so the ImGui Vulkan backend tears down before
`VulkanRenderer::Shutdown()` and the GLFW backend before `mWindow` dies.

**`~Editor()` is a no-op if `Shutdown()` was skipped.** It leaks the ImGui context
rather than reach into a `VulkanRenderer&` it was never given.

**`Editor` is a class rather than free functions**, because it carries state
across calls — the viewport rect, the Style Editor toggle, and whether it is
initialised. As free functions those would be out-parameters threaded through
every call.

Fonts are Inter for body text and Font Awesome 6 for icons, merged into one atlas
and rescaled for the window's `ContentScale()`.

## State

*As of 2026-09-06.* One fixed layout: `Hierarchy` left, `Inspector` right,
`Output` bottom, and the scene showing through the passthru centre. It is built
once and left alone, persisted only through ImGui's own `imgui.ini` next to the
executable.

`Output` is wired to `LogHistory` through `LogPanel`.

No tests. It needs a device and a display.

## Backlog

1. **`Hierarchy` and `Inspector` are docked but empty.** Nothing reads the
   `World` to populate them. The `ComponentRegistry` field tables that
   serialization already uses are the intended route.
2. **Serialized custom layouts**, beyond whatever `imgui.ini` remembers.
3. **Input routing**, beyond what `ImGui_ImplGlfw` gives for free.
4. **Make `Editor` optional on `App`** — a `unique_ptr`, null in a packaged game
   build. This module's boundary is what makes that swap possible without
   touching `RenderSystem` or `VulkanRenderer`.

## Changed

*2026-09-03* — **The module was extracted from `App.cpp`.** Was: about 150 lines
of `ImGui::*` and `ImGui_Impl*` calls sitting next to window and ECS
orchestration, with `App.h` needing dockspace internals just to declare a member.
Now: this module, with a six-call surface.

*2026-09-03* — **The scene rect stopped coming from a window named `"Scene"`.**
Was: read from `ImGui::Begin("Scene")`'s content region, which is the natural
read of the name. Now: read only from the dockspace central node, and the
left-docked window is named `Hierarchy` so that no name implies it is the
viewport.

The original compiled, linked and ran with no warning. The rect was internally
consistent and correctly clamped, and it clipped the scene to a 300x650 sidebar
nowhere near the geometry, so the cube rendered into nothing with nothing to
point at.
