# gameui

Game UI drawn over the 3D scene: rects, textures, text and clipping, in one
area of the window, plus the widget identity, pointer state and focus that
widgets are built on. The same code is a HUD inside the editor's scene viewport and
fullscreen UI in a game build. It owns no ImGui lifetime, opens no window and
holds no layout or widgets yet.

`mir::gameui` · depends `mir::renderer`, `mir::input`, `mir::core` (public); `imgui` (private) · API `modules/gameui/include/gameui/` · maintainer Quy Dong Ta · reviewed 2026-09-30

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

A third slot, focus, is the widget a gamepad or keyboard player is on. There
is one slot for every device, written by whichever moved last. The pointer
writes it when it moves or presses over a widget. `UINavigate`, from the stick,
the D-pad or the arrow keys alike, writes it by following a link. Each widget
names its four neighbours explicitly as `UINavLinks`, up, down, left, right:

```cpp
// a column that wraps; a null UIId is no link that way
const UINavLinks links{UIId("slot", (i + count - 1) % count), UIId("slot", (i + 1) % count), UIId{}, UIId{}};
const UIInteraction interaction = ui.Interact(UIId("slot", i), rect, UIWidgetOptions{.mLinks = links});
if (interaction.mClicked) // a pointer click, or UIConfirm while focused
    Activate(i);
```

Links are not inferred from the rects. Geometry reads well until a layout goes
diagonal or a widget is hidden, and then the call site has no way to correct
it. A link to a hidden or disabled widget passes through it along that widget's
own link in the same direction, so a greyed-out option in the middle of a menu
is stepped over rather than trapping focus. Focus is keyed by `UIId`, so it
stays on its widget however far the widget moves between frames. Like hot, it
settles in `EndFrame` from that frame's declarations, and a widget sees
`mFocused` a frame later.

`UIConfirm` clicks the focused widget, reported through the same `mClicked` as
a pointer click. `UICancel` is the screen's, not a widget's:
`UIContext::CancelPressed()` reports it, and no `UIInteraction` carries it.

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
| `UIState`, `UIPointer`, `UIInteraction` | `gameui/UIState.h` | Hot, active and focus, the press latch, the hit test, and `FollowLink` |
| `UINavLinks`, `UIWidgetOptions` | `gameui/UIState.h` | A widget's four neighbours, and whether it is hidden or disabled |
| `UINavInput`, `UINavDirection` | `gameui/UINavInput.h` | The UI actions for a frame, read from `InputState` by name; `UINavStepper` turns `UINavigate` into steps, one per direction per press |
| `UIContext` | `gameui/UIContext.h` | `BeginFrame(area, InputState&, UIPointer)`/`EndFrame`, `Interact`, `Nav`, `CancelPressed`; owns the draw list, the state and the style |

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
which is how its tests run: `BeginFrame` takes a `UINavInput`, a plain struct.

`UINavigate` steps focus once each time one of its four directions comes on,
so a held stick or key steps once, and holding one arrow while tapping another
steps the tapped way. `UIState`'s `UINavStepper` finds those edges itself from
the action's two axes; the action's own `justPressed` covers all four
directions at once, so it misses the second arrow. A direction comes on when
the input lies in its own quarter, edges included, so two arrows together, a
45 degree diagonal, hold both. It stays on until the input is more than about
63 degrees from it, so a stick pushed right with some drift never steps down,
rolling the stick from right to down steps down once past the diagonal, and a
thumb wobbling on the diagonal does not step again. When two directions come
on in one frame, only the larger axis steps; a tie goes vertical. The stick's
deadzone is its binding's: `InputSystem` has already zeroed a stick inside it,
so gameui sees rest as zero whichever device produced it, and has no deadzone
of its own.

Opposite keys cancel inside the action, which sums its bindings: holding right
and tapping left reads as right let go, and then pressed again, so it steps
right once left comes up. Telling them apart needs per-binding values, which
`InputState` does not carry.

A hidden widget is declared for its links alone. It is not hit and does not
cover what is under it. A disabled widget is drawn, so it is still hit, still
covers what is under it and still reports `mHovered` and `mHeld`. Neither
clicks or takes focus, from the pointer or a link. `FollowLink` stays put when
the chain reaches a null link, a widget not declared this frame, or loops
without reaching an enabled widget. The first `UINavigate` press with nothing
focused, or with the focused widget not declared this frame, lands on the first
enabled widget declared. `UIState::SetFocus` picks the widget a screen opens on.

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

**Declare a hidden widget with `mHidden`; do not skip it.** Links pass through
a hidden widget along its own links, which `UIState` only knows if it was
declared this frame. Skip it and a link to it stops focus dead.

**Links are one-way.** Down from A to B does not make up from B to A; declare
both. Nothing checks that the graph is symmetric.

**Read keys and buttons through the UI actions, never by device.** No `Key`,
`MouseButton`, `GamepadButton` or `DeviceKind` appears in the module, tests
included. `UINavigate`, `UIConfirm`, `UICancel`, `UITabLeft` and `UITabRight`
come from `input/DefaultInputMap.h`, where each is bound on the keyboard, the
mouse and a gamepad, and `ReadNavInput` looks them up by those names. A widget
that branched on the device would ignore the player's rebinds in Project
Settings. The pointer is the exception: it is a position, not an action, and it
is how the mouse navigates, so `UINavigate` has no mouse binding.

**`UIConfirm` while the pointer's button is down, or the frame after, is the
pointer's.** `UIConfirm` is bound to the primary button too, and `InputState`
is a frame behind the pointer, so every mouse click would otherwise also arrive
as a confirm a frame later and activate the focused widget a second time, or
the focused widget from a click on empty space. `UIState` drops those; the
latch reports the pointer's click on release. A keyboard or pad confirm pressed
while the mouse button is held is dropped with them.

**Build the pointer from the window's raw input, not `InputState`.** `InputState`
has no pointer position, and it is a frame old when the UI runs. `App` reads
`Window::RawInput()`, which `PollEvents` has just refreshed.

## State

*As of 2026-09-30.* A vertical slice. `App` draws three rects and strings
through it (`DrawPlaceholderHud` in `App.cpp`), in the scene viewport with the
editor layout and fullscreen with `--no-editor`. The rects are a column of
widgets linked up and down with wrap: each brightens when focused, brightens
further while held, and logs a line when clicked, and `UICancel` logs one for
the screen. That is the check that hit testing follows the viewport around the
dock, and that hover, the stick, the D-pad and the arrow keys share focus.

Widget identity, hot and active, hit testing, focus and explicit links are
done. There are no widgets yet; the first one is built on `Interact`.

`Texture` records but draws a flat tint, and warns once: the renderer has no
`TextureHandle` to `ImTextureID` bridge yet.

`UIContext::Nav()` holds the UI actions read at `BeginFrame`; `UIState` reads
`UINavigate` and `UIConfirm` from them, and `UITabLeft` and `UITabRight` are
unused yet. `App` merges `DefaultInputMap()` into the project's map at
startup with `AddDefaultActions`, so they are in every project and appear in
Project Settings; a rebound action keeps its bindings, and a removed one comes
back at the next start. `UIContext::Input()` hands back the `InputState` from
`BeginFrame`. It is a frame old at that point, since `InputSystem` runs later in
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
duplicate ids. `tests/UIFocus.tests.cpp` covers `UINavStepper` (one step per hold, a tap
under a held arrow, stick drift, and a roll and wobble across the diagonal), link
traversal (skipping hidden and disabled widgets, a link to an undeclared
widget, a wrapping cycle and cycles with nothing to land on), hover against
`UINavigate` in the one slot, focus surviving a move, and `UIConfirm`,
including a mouse click not confirming twice. The backend needs a display and is verified by running.

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
6. **Repeat while held.** A held direction steps once. Menus usually repeat
   after a delay and then at a rate; that fits in `UINavStepper`, which would
   then need the frame's time.
7. **Focus scopes.** One focus slot for the whole UI. A modal over a menu
   needs to save and restore it, which is `SetFocus` by hand today.
8. **Id scopes.** ImGui's id stack, where a parent's id seeds its children's, so
   two panels can each hold a "close" button without naming them apart. Not
   needed until a widget is made of other widgets; the first composite widget
   settles whether a stack or an explicit parent argument reads better.

## Changed

*2026-09-30* — **`Interact` takes `UIWidgetOptions`, and `UIState::BeginFrame`
a `UINavInput`.** Was: `Interact(id, rect)` and `UIState::BeginFrame(area,
pointer)`. Now: `Interact(id, rect, options = {})` with the widget's links and
its hidden and disabled flags, and `UIState::BeginFrame(area, pointer, nav =
{})`. Both default, so existing calls compile unchanged. `UIInteraction` gains
`mFocused`, and `mClicked` is also set by `UIConfirm` on the focused widget.
Because focus needs each widget's links and whether it can take focus, and
`UIState` needs the frame's UI actions to move it.

*2026-09-28* — **`UIContext::BeginFrame` takes a `UIPointer`.** Was:
`BeginFrame(area, InputState&)`, and nothing read input. Now:
`BeginFrame(area, InputState&, UIPointer)`, with the pointer in window pixels.
Because hit testing needs the pointer's position, which `InputState` does not
carry, and `InputState` is a frame old by the time the UI runs.
