# gameui

Game UI drawn over the 3D scene: rects, textures, text and clipping, in one
area of the window, plus the widget identity and pointer state that widgets
are built on. The same code is a HUD inside the editor's scene viewport and
fullscreen UI in a game build. It owns no ImGui lifetime, opens no window and
holds no layout or widgets yet.

`mir::gameui` · depends `mir::renderer`, `mir::input`, `mir::core` (public); `imgui` (private) · API `modules/gameui/include/gameui/` · maintainer Quy Dong Ta · reviewed 2026-09-28

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
mGameUI.BeginFrame(uiArea, mWorld.Resource<InputState>(), ToUIPointer(mWindow->RawInput()));
DrawPlaceholderHud(mGameUI);
mGameUI.EndFrame();
```

A widget is a rect plus a `UIId`. The id is a hash of a name and an optional
index, the trick ImGui uses, so a widget in a loop keeps its identity
across frames without its call site storing one, and keeps it wherever it is
drawn. `UIContext::Interact(id, rect)` hit-tests the rect against the pointer
under the draw list's current clip and reports what the pointer did to it:

```cpp
// DrawPlaceholderHud in App.cpp; a widget in a loop would use UIId("slot", i)
const UIInteraction interaction = ui.Interact(UIId("placeholder_hud"), panel);
if (interaction.mClicked)
    MIR_LOG_INFO("gameui: placeholder panel clicked");
```

`UIState` holds two slots. Hot is the widget under the pointer. Active is the
widget the pointer was pressed on, latched until the button comes up, however
far the pointer moves. A click is a release while the pointer is over the
active widget, so press, drag away, release does nothing, as it does on every
desktop button. Both slots settle in `EndFrame` from every widget declared that
frame. That is what lets the widget drawn last, which is on top, win where
widgets overlap, and it is why a widget sees its hover and press a frame late.
The click itself is reported in the frame of the release.

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
| `UIRect`, `UIColor`, `UIStyle` | `gameui/UITypes.h` | Value types; `Intersect` and `Contains` for rects |
| `UIDrawList` | `gameui/UIDrawList.h` | `Rect`, `Texture`, `Text`, `PushClip`/`PopClip`, recorded as `UIDrawCommand`s |
| `UIId` | `gameui/UIId.h` | A widget's identity: a `constexpr` hash of a name and an optional index |
| `UIState`, `UIPointer`, `UIInteraction` | `gameui/UIState.h` | Hot and active, the press latch, and the hit test |
| `UIContext` | `gameui/UIContext.h` | `BeginFrame(area, InputState&, UIPointer)`/`EndFrame`, `Interact`; owns the draw list, the state and the style |

Coordinates passed to `UIDrawList` and to `Interact` are local: `(0, 0)` is the
top-left of the area. Each command stores its clip already intersected with
every clip around it, so the backend never tracks a stack. A draw entirely
outside the clip is not recorded; text is not measured, so only an empty clip
culls it.

The pointer, like the area, is in window pixels. `UIState::BeginFrame` subtracts
the area's origin once, so the hit test is right wherever the dock layout puts
the scene viewport. A hit must fall inside the widget's rect, the clip and the
area, with right and bottom edges outside, so a widget that overhangs the
viewport cannot be hit through an editor panel beside it.

`UIId("slot")` and `UIId("slot", 0)` are different ids, and so are
`UIId("item", 1)` and `UIId("item1")`: the index is mixed into the hash, not
appended to the name. `UIState` can be driven without `UIContext` or a display,
which is how its tests run.

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

**Give each widget its own id within a frame.** Two widgets declared with one id
are one widget to `UIState`: pressing either holds both. It is not an assert,
because it is easy to do by accident and the result is still usable, but
`UIState::Collisions()` counts it and each colliding id is logged once. In a
loop, pass the index.

**Declare a widget in the same order it is drawn.** Hot goes to the last widget
declared under the pointer, on the assumption that it was drawn last and is on
top. Declaring in a different order from drawing lets a covered widget take the
click.

**Declare a held widget every frame.** An active widget that is not declared in
a frame is let go at `EndFrame`, so one that is skipped for a frame, or
disappears, while held never clicks.

**Build the pointer from the window's raw input, not `InputState`.** `InputState`
has no pointer position, and it is a frame old when the UI runs. `App` reads
`Window::RawInput()`, which `PollEvents` has just refreshed.

## State

*As of 2026-09-28.* A vertical slice. `App` draws one rect and one string
through it (`DrawPlaceholderHud` in `App.cpp`), in the scene viewport with the
editor layout and fullscreen with `--no-editor`. The rect is declared as a
widget: it brightens when hot, brightens further while held, and logs a line
when clicked. That is the check that hit testing follows the viewport around
the dock.

Widget identity, hot and active, and hit testing are done. There are no
widgets yet; the first one is built on `Interact`.

`Texture` records but draws a flat tint, and warns once: the renderer has no
`TextureHandle` to `ImTextureID` bridge yet.

`UIContext::Input()` hands back the `InputState` from `BeginFrame`, but nothing
reads it. It is a frame old at that point, since `InputSystem` runs later in
`PreUpdate`.

A click in the viewport reaches the game UI and the editor alike. In Edit mode
a game widget over a mesh is pressed and the mesh is picked, and an editor
popup or menu open over the viewport does not shield the widget under it. See
the backlog.

`tests/UIDrawList.tests.cpp` covers recording, the clip stack, culling and the
text buffer. `tests/UIId.tests.cpp` covers hashing: stable ids, index mixing and
no collisions across ten thousand names or indices. `tests/UIState.tests.cpp`
covers the latch (press, drag away, release; drag back; a press from outside),
overlap, a widget that moves while held, the clip stack, an offset area and
duplicate ids. The backend needs a display and is verified by running.

## Backlog

1. **Textures.** A renderer call that registers a `TextureHandle` with the ImGui
   Vulkan backend, so `Texture` draws the texture.
2. **Pointer ownership.** Whether the game UI or the editor owns a click in the
   viewport: a hot game widget should stop the editor picking under it, and an
   editor popup over the viewport should hide the pointer from the game UI.
   Belongs with `imguiruntime`'s input-capture item. Hit testing itself landed
   2026-09-28.
3. **Content from the game.** Replace `DrawPlaceholderHud` with UI a game
   authors, through Lua or data.
4. **Text measurement**, for layout and for culling text by its bounds.
5. **DPI.** Coordinates are framebuffer pixels, as the editor's viewport rect
   is; nothing scales with the monitor yet.
6. **Id scopes.** ImGui's id stack, where a parent's id seeds its children's, so
   two panels can each hold a "close" button without naming them apart. Not
   needed until a widget is made of other widgets; the first composite widget
   settles whether a stack or an explicit parent argument reads better.

## Changed

*2026-09-28* — **`UIContext::BeginFrame` takes a `UIPointer`.** Was:
`BeginFrame(area, InputState&)`, and nothing read input. Now:
`BeginFrame(area, InputState&, UIPointer)`, with the pointer in window pixels.
Because hit testing needs the pointer's position, which `InputState` does not
carry, and `InputState` is a frame old by the time the UI runs.
