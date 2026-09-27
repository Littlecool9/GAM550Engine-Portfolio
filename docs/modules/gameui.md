# gameui

Game UI drawn over the 3D scene: rects, textures, text and clipping, in one
area of the window. The same code is a HUD inside the editor's scene viewport
and fullscreen UI in a game build. It owns no ImGui lifetime, opens no window
and holds no layout or widgets yet.

`mir::gameui` · depends `mir::renderer`, `mir::input`, `mir::core` (public); `imgui` (private) · API `modules/gameui/include/gameui/` · maintainer Quy Dong Ta · reviewed 2026-09-27

## Model

A `UIContext` brackets one frame of game UI inside `imguiruntime`'s frame. Its
`UIDrawList` records commands in coordinates local to the area given to
`BeginFrame`, and `EndFrame` replays them into ImGui's background draw list,
offset to that area and clipped to it.

```cpp
// App::Run, after Editor::DrawLayout has set this frame's scene viewport
const UIRect uiArea = mDesc.mEnableEditorLayout
    ? ToUIRect(mEditor.SceneViewportRect())
    : UIRect{0.0f, 0.0f, static_cast<float>(mWindow->Width()), static_cast<float>(mWindow->Height())};
mGameUI.BeginFrame(uiArea, mWorld.Resource<InputState>());
DrawPlaceholderHud(mGameUI);
mGameUI.EndFrame();
```

The background list is why one code path serves both builds. ImGui renders it
before every window, and `VulkanRenderer` draws ImGui after the 3D views, so it
lands over the scene and under Hierarchy, Inspector and Output. The dockspace
passes its central node through, so nothing covers the viewport. No renderer
change was needed.

Recording, not drawing immediately, keeps ImGui in one `.cpp` and lets the
clip stack be tested without a display.

## API

| Type | Header | Role |
|---|---|---|
| `UIRect`, `UIColor`, `UIStyle` | `gameui/UITypes.h` | Value types; `Intersect` for rects |
| `UIDrawList` | `gameui/UIDrawList.h` | `Rect`, `Texture`, `Text`, `PushClip`/`PopClip`, recorded as `UIDrawCommand`s |
| `UIContext` | `gameui/UIContext.h` | `BeginFrame(area, InputState&)`/`EndFrame`, owns the draw list and style |

Coordinates passed to `UIDrawList` are local: `(0, 0)` is the top-left of the
area. Each command stores its clip already intersected with every clip around
it, so the backend never tracks a stack. A draw entirely outside the clip is
not recorded; text is not measured, so only an empty clip culls it.

## Rules

**No public header names ImGui, and none contains the string `Im`.** `grep -rn
"Im" modules/gameui/include/gameui/` must print nothing. It is why the image
primitive is `Texture`, not `Image`. A public ImGui type would make `imgui` a
public dependency of everything that draws game UI, and tie the game's API to
the editor's library.

**Only `src/gameui/UIBackendImGui.cpp` includes `<imgui.h>`.** `UIContext.cpp`
reaches it through `detail::SubmitToBackground` in the private `UIBackend.h`.
Swapping the backend means replacing that one file.

**No `ImGui::Begin` anywhere in the module.** A window would sit among the
editor's panels and dock like one. The background list has no window.

**Draw after `Editor::DrawLayout` and before `ImGuiRuntime::EndFrame`.**
`SceneViewportRect()` is set inside `DrawLayout`, so drawing earlier uses last
frame's viewport and lags a dock drag by a frame. After `EndFrame` the draw data
is already built and the UI never appears.

**The fullscreen area is the window's framebuffer size, not the swapchain's.**
The swapchain catches up to a resize only inside `DrawFrame`, a frame after the
UI is built, so its extent would clip the first frame of every resize.

**Balance every `PushClip`.** `EndFrame` asserts on an unpopped clip, and
`PopClip` asserts on popping the root.

## State

*As of 2026-09-27.* A vertical slice. `App` draws one rect and one string
through it (`DrawPlaceholderHud` in `App.cpp`), in the scene viewport with the
editor layout and fullscreen with `--no-editor`.

`Texture` records but draws a flat tint, and warns once: the renderer has no
`TextureHandle` to `ImTextureID` bridge yet.

`UIContext::Input()` hands back the `InputState` from `BeginFrame`, but nothing
reads it. It is a frame old at that point, since `InputSystem` runs later in
`PreUpdate`.

`tests/UIDrawList.tests.cpp` covers recording, the clip stack, culling and the
text buffer. The backend needs a display and is verified by running.

## Backlog

1. **Textures.** A renderer call that registers a `TextureHandle` with the ImGui
   Vulkan backend, so `Texture` draws the texture.
2. **Input.** Hit-testing against `InputState`, and whether the game UI or the
   editor owns a click in the viewport. Belongs with `imguiruntime`'s
   input-capture item.
3. **Content from the game.** Replace `DrawPlaceholderHud` with UI a game
   authors, through Lua or data.
4. **Text measurement**, for layout and for culling text by its bounds.
5. **DPI.** Coordinates are framebuffer pixels, as the editor's viewport rect
   is; nothing scales with the monitor yet.
