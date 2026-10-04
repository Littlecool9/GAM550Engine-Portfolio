# gameui

Game UI drawn over the 3D scene: rects, textures, nine-sliced sprites, text
and clipping, in one area of the window, plus the widget identity, pointer
state and focus that widgets are built on, the core widgets, in grey box or in
art the style supplies, and helpers that lay them out inside a safe area. The
same code is a HUD inside the editor's scene viewport and fullscreen UI in a
game build. It owns no ImGui lifetime, opens no window, reads no file and keeps
no layout between frames.

`mir::gameui` · depends `mir::assets`, `mir::input`, `mir::core` (public); `imgui` (private) · API `modules/gameui/include/gameui/` · maintainer Quy Dong Ta · reviewed 2026-10-02

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
mGameUI.BeginFrame(uiArea, mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(),
                   ToUIPointer(mWindow->RawInput()));
GameShellAction shellAction = GameShellAction::None;
if (mDesc.mUIDemo)
    mUIDemo.Draw(mGameUI);
else
    shellAction = mGameShell.Draw(mGameUI, IsPlaying(mWorld), dt);
mGameUI.EndFrame();
```

A widget is a rect plus a `UIId`. The id is a hash of a name and an optional
index, the trick ImGui uses, so a widget in a loop keeps its identity
across frames without its call site storing one, and keeps it wherever it is
drawn. `UIContext::Interact(id, rect)` hit-tests the rect against the pointer
under the draw list's current clip and reports what the pointer did to it:

```cpp
// what every widget in UIWidgets.cpp does first; one in a loop would use UIId("slot", i)
const UIInteraction interaction = ui.Interact(UIId("play"), rect);
if (interaction.mClicked)
    StartGame();
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
a pointer click. `UICancel` and `UIPause` are the screen's, not a widget's:
`UIContext::CancelPressed()` and `PausePressed()` report them, and no
`UIInteraction` carries them.

A widget declared `mPointerOnly` is clicked by the pointer and never focused.
No link, hover, press or first `UINavigate` press lands on it, and links pass
over it as over a disabled one. It is for a HUD button in play, where the stick
and the confirm button belong to the game: a focusable one would catch the
first push of the stick and then the jump button.

A screen that takes the player's input from the game while it is open, such
as a pause menu, calls `UIContext::HoldInput()` in each frame it is open.
`EndFrame` then writes `InputState::SetGameInputHeld`, and gameplay, which
reads `InputState::ForGame`, sees every action at rest. The first frame no
screen calls it, the game has its input back. gameui's own reads go on through
`Get`, and no device is dropped, so the screen holding the input is still
driven by every device. That is the difference from
`InputState::SetUiCapture`, the editor's: it drops the keyboard or the mouse
for every reader, so a menu that set it would lock itself out of the very keys
it is navigated by.

Two things are decided by `InputState::ActiveDevice()`, the device that last
spoke, and nothing else in gameui reads it. The first is whether the focus
highlight is drawn. Under the mouse it is noise, since the pointer already
shows where the player is, so `UIContext::BeginFrame` hides it while the mouse
is the active device and shows it under a gamepad or the keyboard. Focus itself
is kept either way: `mFocused` is unchanged, and a widget draws its highlight
from `UIInteraction::mShowFocus`, which is `mFocused` while the highlight is
shown. The press that brings the highlight back only brings it back. It shows
on the frame that press reaches the UI, on the widget that was focused, and
neither steps nor confirms, so switching devices mid-menu never moves focus or
activates a widget the player could not see. With nothing focused, a
`UINavigate` press still lands on the first enabled widget.

The second is which label a button prompt shows. `UIContext::Prompt(action)`
returns a `UIPrompt`: the short name of the action's binding on the active
device, `A` or `LB` on a gamepad and `Enter` or `Escape` on the keyboard, with
`mGamepad` set for a gamepad face button. The keyboard and the mouse are one
side, so moving the mouse does not turn `Enter` into `LMB`; an action with no
keyboard binding shows its mouse one. The name comes from `PromptLabel` in
`input/PromptLabel.h`, which reads the action's bindings, so a rebind in
Project Settings changes the prompt, and gameui still names no key or button.

`InputSystem` makes the mouse the active device when the cursor moves, as well
as on a button or scroll. A gamepad, then a held key, still win over it in the
same frame.

### Widgets

`gameui/UIWidgets.h` has six widgets as free functions over a `UIContext`:

```cpp
if (Button(ui, UIId("play"), rect, "Play", {.mLinks = links}).mClicked)
    StartGame();
Toggle(ui, UIId("subtitles"), rect, "Subtitles", settings.mSubtitles);
if (Slider(ui, UIId("volume"), rect, settings.mVolume, 0.0f, 1.0f, 0.05f).mChanged)
    ApplyVolume(settings.mVolume);
Bar(ui, UIId("health"), rect, health / maxHealth);
```

Each calls `Interact` once and draws its parts, a face, a track, a fill or a
focus ring, as grey-box rects or as the style's sprites, and then its text. The
backend turns those into `AddRectFilled`, `AddImage` and `AddText`. Each returns a
`UIWidgetResult`, the frame's `UIInteraction` plus `mChanged` when it wrote the
caller's value. None keeps anything between frames: the caller owns the value,
passed by reference to `Toggle` and `Slider`, and `UIState` owns hot, active
and focus. So a widget can be skipped, moved or redrawn from another call site
without anything going stale, and art replaces the grey box at the same call
sites, through the style (see Art below).

`Button` clicks, and `Toggle` flips its `bool`, on a pointer click or on
`UIConfirm` while focused. `Slider` follows a press and drag across its rect,
latched like any press, so the drag keeps going outside it. While it is focused,
`UINavigate` left and right move it by its step. That comes from `UIState`, not
the slider: a widget declared with `UIWidgetOptions::mAdjustsX` gets a left or
right step as `UIInteraction::mAdjust`, -1 or +1, instead of focus following its
left or right link, and up and down still navigate. A step above 0 also snaps a
drag; step 0 drags smoothly and steps a twentieth of the range.

`Label`, `Image` and `Bar` show something rather than take input, so they are
inert unless `UIDisplayOptions::mFocusable`. Inert, they are declared disabled:
hovered and covering what is under them, never focused or clicked, and links
pass over them. The pointer crossing a caption therefore leaves focus on the
button beside it. A focusable one clicks and takes focus like a button.

Every widget draws its focus ring, a rect `mFocusThickness` larger behind it,
from `mShowFocus`, so the ring follows the active device. Colours, sprites,
rounding and padding are in `UIStyle`. Text is left-aligned after the padding and centred by
`mTextHeight`, since nothing measures it yet.

The background list is why one code path serves both builds. ImGui renders it
before every window, and `VulkanRenderer` draws ImGui after the 3D views, so it
lands over the scene and under Hierarchy, Inspector and Output. The dockspace
passes its central node through, so nothing covers the viewport. No renderer
change was needed.

Recording, not drawing immediately, keeps ImGui in one `.cpp` and lets the
clip stack be tested without a display.

### Art

A widget goes from grey box to art in `UIStyle`, and never at its call site.
Each grey-box colour has a `UISprite` beside it: `mPanelSprite`,
`mWidgetSprite` with `mWidgetHoveredSprite` and `mWidgetHeldSprite`,
`mTrackSprite`, `mFillSprite` and `mFocusSprite`. A null sprite draws the
colour. A set one is drawn untinted in its place. A null hovered or held
sprite falls back to `mWidgetSprite`, not to its colour, so art with no hover
frame stays art while hovered.

```cpp
// UIDemoScreen::Load; the Button and Toggle calls in Draw do not change
UISprite button = ui.Textures().Load(*assets, MakeAssetId("assets/ui/button.png"));
button.mSlice = UIEdges{8, 8, 8, 8};
ui.Style().mWidgetSprite = button;
```

A `UISprite` holds a `UITexture`, the texture's size, the source rect in
texels, which is the whole texture or one cell of an atlas, and `mSlice`: how
many texels in from each side are corner and edge. `UIDrawList::Sprite`
records it as up to nine `Texture` commands. The corners keep their texel size
whatever the rect's size. The top and bottom edges stretch across, the left and
right edges stretch down, and the middle stretches both ways. A rect too small
for two corners shrinks all four by one factor, so they meet without changing
shape instead of overlapping. A zero slice stretches the whole source, which is
how `Image` draws a plain picture.

Textures come in cooked. `AssetCooker` decodes every standalone `.png`, `.jpg`
and `.jpeg` under a source root to a `TextureAsset`, RGBA8, under the id of its
own path, so the runtime never parses an image format. An image a glTF
references is left to that model's import. `UITextures::Load(AssetCache&,
AssetId)` reads the payload through the normal asset path, and loading an id
twice returns the same texture.

`UITextures`, which `UIContext` owns, hands each texture to ImGui 1.92's
dynamic texture path. The pixels are copied into an `ImTextureData` in
`WantCreate`, which is registered as a user texture. ImGui lists it to the
renderer backend every frame. `ImGui_ImplVulkan` creates the image, uploads
it and makes its descriptor on the first frame it renders, and frees all of it
in its own shutdown. `AddImage` takes the `ImTextureData` itself, so no
descriptor is made by hand and the renderer has no part in it, which is why
gameui does not link `renderer`.

### Layout

`gameui/UILayout.h` turns one rect into others. `Inset` takes edges in,
`Anchor` puts a rect of a given size on the same point of a container (a
corner, an edge's middle or the centre), and `UIStack` cuts a row across a rect
or a column down it, a gap apart. `Next` cuts from the start, `Last` from the
end and `Rest` takes what lies between, so a row keeps a label at its right
whatever is on its left. Each helper takes a rect and returns rects, so they
nest in any order, and none keeps anything, so a layout is worked out again
every frame from the area it is given and follows a resize or a dock drag with
no event.

```cpp
// UIDemoScreen::Layout: a row inside a column inside an anchored region
UIStack screen = UIStack::Column(Inset(safeArea, kMargin), kGap);
layout.mPrompts = screen.Last(kPromptHeight);
const UIRect above = screen.Rest();

const float width = std::min(std::clamp(above.width * kMenuShare, kMenuMinWidth, kMenuMaxWidth), above.width);
layout.mPanel = Anchor(above, kUILeft, width, kMenuHeight);

UIStack menu = UIStack::Column(Inset(layout.mPanel, kPadding), kGap);
layout.mTitle = menu.Next(kRow);
UIStack sliderRow = UIStack::Row(menu.Next(kSliderRow), kGap);
layout.mVolume = sliderRow.Last(kVolumeWidth);
layout.mSlider = sliderRow.Rest();
```

A screen is laid out in `UIContext::SafeArea()`, its `Bounds()` less the
safe-area insets. The insets are a setting, `SetSafeAreaInsets`, in pixels and
kept across frames: a TV's title-safe margin, a notch, or a player's overscan
choice, which whoever owns the screen sets and rescales on a resize. They are
zero until set, so the safe area is the whole area. Nothing in a screen names
an inset, so the same screen fits a television and a monitor.

A cut is clamped to what is left, so nothing a stack lays out leaves its rect.
A stack that runs out hands out empty rects, which draw nothing and are never
hit, rather than spilling over its neighbours. `UIStack::Length` gives the
length a stack of given sizes needs, gaps included, to size a region that hugs
its contents, as the demo menu's height does.

Sizes stay the caller's: a fixed size, a share of the container clamped to
limits, or whatever `Rest` leaves. There is no flex, wrap or grid, and no
measuring pass, since nothing measures text yet.

## API

| Type | Header | Role |
|---|---|---|
| `UIRect`, `UIColor`, `UIEdges`, `UITexture`, `UISprite`, `UIStyle` | `gameui/UITypes.h` | Value types; `Intersect` and `Contains` for rects; a sprite is a texture, a source rect and a nine-slice |
| `UIDrawList` | `gameui/UIDrawList.h` | `Rect`, `Texture`, `Sprite`, `Text`, `PushClip`/`PopClip`, recorded as `UIDrawCommand`s; `Sprite` nine-slices into `Texture`s |
| `UITextures` | `gameui/UITextures.h` | Cooked textures as ImGui user textures, handed out as sprites; `Load` from an `AssetCache`, `Add` from a `TextureAsset` |
| `UIId` | `gameui/UIId.h` | A widget's identity: a `constexpr` hash of a name and an optional index |
| `UIState`, `UIPointer`, `UIInteraction` | `gameui/UIState.h` | Hot, active and focus, the press latch, the hit test, and `FollowLink` |
| `UINavLinks`, `UIWidgetOptions` | `gameui/UIState.h` | A widget's four neighbours, whether it is hidden or disabled, whether it takes left and right (`mAdjustsX`), and whether only the pointer reaches it (`mPointerOnly`) |
| `UINavInput`, `UINavDirection` | `gameui/UINavInput.h` | The UI actions for a frame, read from `InputState` by name; `UINavStepper` turns `UINavigate` into steps, one per direction per press |
| `UIWidgetResult`, `UIDisplayOptions`; `Label`, `Button`, `Image`, `Bar`, `Toggle`, `Slider`; `Panel` | `gameui/UIWidgets.h` | The core widgets, in grey box or the style's art; `Panel` draws the style's panel and declares nothing |
| `UIAnchor`, `UIAxis`, `UIStack`; `Inset`, `Anchor` | `gameui/UILayout.h` | Layout over rects: insets, anchoring to a point of a container, rows and columns cut from either end; `kUITopLeft` to `kUIBottomRight` |
| `UIDemoScreen`, `UIDemoLayout` | `gameui/UIDemoScreen.h` | Every core widget on one screen, drawn by `App` under `--ui-demo`; `Layout(safeArea)` places it without drawing |
| `UIContext`, `UIPrompt` | `gameui/UIContext.h` | `BeginFrame(area, InputState&, InputMap&, UIPointer)`/`EndFrame`, `Interact`, `Nav`, `CancelPressed`, `PausePressed`, `HoldInput`/`HoldsInput`, `FocusVisible`, `Prompt`, `Bounds`, `SafeArea`/`SetSafeAreaInsets`; owns the draw list, the state, the style, the textures and the safe-area insets |

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

**No public header names ImGui.** `grep -rnE "ImGui|Im[A-Z]|imgui"
modules/gameui/include/gameui/` must print nothing, which catches every ImGui
type (`ImVec2`, `ImU32`, `ImDrawList`, `ImTextureID`) and the header itself
while leaving words such as `Image`. A public ImGui type would make `imgui` a
public dependency of everything that draws game UI, and tie the game's API to
the editor's library.

**Only `src/gameui/UIBackendImGui.cpp` includes `<imgui.h>`.** `UIContext.cpp`
and `UITextures.cpp` reach it through `detail::SubmitToBackground` and
`detail::CreateBackendTexture` in the private `UIBackend.h`. It also includes
`<imgui_internal.h>`, for `RegisterUserTexture`. Swapping the backend means
replacing that one file, and the two test fixtures that host a headless ImGui
context: `tests/UIWidgets.tests.cpp` and `tests/UITextures.tests.cpp` include
`<imgui.h>` for that alone, so `EndFrame`'s real submit and the textures run
under test.

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
`MouseButton` or `GamepadButton` appears in the module, tests included, and
`DeviceKind` only at the two reads of `ActiveDevice()`, `UIContext::BeginFrame`
and `UIContext::Prompt`, and in their tests. A third read is a widget deciding
by device; it belongs in one of those two, or as a new decision beside them.
`UINavigate`, `UIConfirm`, `UICancel`, `UITabLeft`, `UITabRight` and
`UIPause` come from `input/DefaultInputMap.h`, where each is bound on the
keyboard, the mouse and a gamepad, and `ReadNavInput` looks them up by those
names. A widget that branched on the device would ignore the player's rebinds in
Project Settings. The pointer is the exception: it is a position, not an
action, and it is how the mouse navigates and pauses, so `UINavigate` and
`UIPause` have no mouse binding; the mouse pauses by clicking the HUD's pause
button.

**Take the game's input with `HoldInput`, never `SetUiCapture`.** The capture
flags are the editor's, set by `App` from ImGui each frame, and they drop a
device for every reader, the game UI's own actions included. A screen that set
them would stop answering the keyboard or the mouse it is driven by, and
`App` would overwrite them the same frame anyway. `HoldInput` leaves every
device to the game UI and hides the actions from gameplay alone.

**`UIConfirm` while the pointer's button is down, or the frame after, is the
pointer's.** `UIConfirm` is bound to the primary button too, and `InputState`
is a frame behind the pointer, so every mouse click would otherwise also arrive
as a confirm a frame later and activate the focused widget a second time, or
the focused widget from a click on empty space. `UIState` drops those; the
latch reports the pointer's click on release. A keyboard or pad confirm pressed
while the mouse button is held is dropped with them.

**Give a widget's look to `UIStyle`, never to its call.** No widget takes a
sprite for its face, track, fill or ring. A sprite argument on `Button` would
put the art at every call site, and reskinning a game would mean editing every
screen. `Image` alone takes one, because its picture is its content rather than
its look.

**gameui reads no file.** Textures arrive through `AssetCache` as cooked
`TextureAsset`s. An image decoder at runtime would be a second image path beside
the cooker's, with its own failures, in every game build.

**Destroy `UIContext` after `ImGuiRuntime::Shutdown`.** A texture's GPU side
belongs to ImGui's Vulkan backend, which frees a registered texture only while
rendering or in its own shutdown. A `UITextures` destroyed after the context
frees only CPU memory. One destroyed while the context lives unregisters every
texture the backend never uploaded, and leaks any it did, with an error,
rather than free memory the backend still holds. `App` holds `mGameUI` as a
member and does not reset it in `Shutdown`, so it goes with `App`, after the
context.

**Lay a screen out in `SafeArea()`, never in `Bounds()` or by constant.** The
safe area is the one place the display's and the player's insets reach a
screen. A screen that starts from `Bounds()`, or from a hard-coded margin
standing in for an inset, puts its prompts under a TV's overscan or a notch,
and no setting can bring them back. A screen's own margins are fine, inside the
safe area.

**Build the pointer from the window's raw input, not `InputState`.** `InputState`
has no pointer position, and it is a frame old when the UI runs. `App` reads
`Window::RawInput()`, which `PollEvents` has just refreshed.

## State

*As of 2026-10-02.* `App` runs the game UI every frame, in the scene viewport
with the editor layout and fullscreen with `--no-editor` or in a build without
the editor (`windows-msvc-game-*`), where it draws with no editor present: no
theme, docking or DPI scale on the ImGui side, and the same submit. It draws the game
shell from [gameshell.md](gameshell.md): a HUD and a pause menu, built on this
module's public API alone, which show only in Play. Under `--ui-demo` it draws
`UIDemoScreen` in their place. `App` calls
`UIDemoScreen::Load` at startup under the same flag, which loads the art in
`assets/ui/`. It lays itself out with the layout helpers: a menu anchored to
the left of the safe area, as tall as its rows and 30% as wide within 320 to
480 pixels, over a strip of prompts cut from the bottom, at any aspect ratio.
The menu has a `Panel` behind it; a title `Label`; a `Button`
that counts its clicks; a `Toggle` that swaps the style between that art and
grey box; a disabled `Button` that up and down pass over; a `Slider` with a
`Label` showing its percentage; a `Bar` driven by the slider; the icon as an
`Image` tinted by the toggle. The Confirm and Back prompts sit at the bottom
right, a pill with the gamepad button or a key cap with the key, sized by a
guess at the label's width. The button, toggle and slider are a column
linked up and down with wrap, and focus starts on the button. Clicks, toggles
and `UICancel` log a line. That is the check that hit testing follows the
viewport around the dock, and that hover, the stick, the D-pad and the arrow
keys share focus.

Widget identity, hot and active, hit testing, focus and explicit links, the
active-device focus highlight and prompt labels, the six core widgets in grey
box, textures, nine-slice and style art, and the layout helpers with a safe
area are done. `App` never sets the safe-area insets, so they are zero. The art in `assets/ui/`
is generated placeholder: a panel, a button in three states, a track, a fill, a
focus ring and an icon.

`UIContext::Nav()` holds the UI actions read at `BeginFrame`; `UIState` reads
`UINavigate` and `UIConfirm` from them, the pause menu reads `UIPause` and
`UICancel`, and `UITabLeft` and `UITabRight` are unused yet. `App` merges `DefaultInputMap()` into the project's map at
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
including a mouse click not confirming twice, and the focus highlight hiding
and coming back without moving focus or confirming.
`tests/UIActiveDevice.tests.cpp` drives `UIContext` with an empty area, so no
backend is needed, and covers the highlight and the prompt per device, a
device switch mid-menu, and `HoldInput` holding the game's input from the frame
it is called in until a frame does not call it, leaving the capture flags
alone. `tests/UIFocus.tests.cpp` covers a pointer-only widget clicked by the
pointer and never focused, and links passing over it. `tests/UIFocus.tests.cpp` also covers `mAdjustsX`:
left and right adjust the focused widget and leave focus, up and down still
navigate, and the press that brings the highlight back adjusts nothing.
`tests/UIWidgets.tests.cpp` runs a headless ImGui context, so `EndFrame`'s
submit to the backend runs too, and covers each widget's result by pointer
and by UI action, the slider by drag (snapping, clamping, the latch outside
the rect) and by step (once per press, clamped, a disabled slider), the display
widgets staying inert unless focusable, what the bar and the focus ring draw,
a hidden widget drawing nothing while its links still pass, and the demo
screen walked by gamepad, and a widget going from grey box to art, and back,
by its style alone, with every part's sprite and the hovered and held
fallbacks. `tests/UIDrawList.tests.cpp` also covers nine-slice: corners kept
across sizes, pieces covering the rect exactly, corners shrinking by one factor,
uneven and oversized slices, an atlas cell, and culling by piece.
`tests/UITextures.tests.cpp` hosts a headless ImGui context and covers a texture
reaching ImGui's list in `WantCreate` with its pixels, loading through an
`AssetCache` (once per id, and a null sprite for a raw or missing asset),
teardown before and after the context, and `EndFrame` drawing a sprite with
the `ImTextureData` as its texture. `tests/UILayout.tests.cpp` covers each
helper, a row inside a column inside an anchored region, and the demo's
`Layout` at 16:9, 21:9, 4:3 and three more sizes, each with no inset, a TV's 5%
and a lopsided one: everything inside the safe area, the menu down its left and
centred above the prompts, its rows in order, and the prompts along the bottom
clear of it. `tests/UIWidgets.tests.cpp` draws the demo through `UIContext` at
16:9, 21:9 and 4:3 and checks every command it records lands inside
`SafeArea()`, and that the insets move the menu. `smoke.ui-demo` runs `--ui-demo` on the
GPU with validation, which uploads and frees the art. That the backend's output
looks right is verified by running `--ui-demo`.

## Backlog

1. ~~**Textures.**~~ Landed 2026-10-02, through ImGui's dynamic texture path
   rather than a renderer bridge. See Art.
2. **Pointer ownership.** Whether the game UI or the editor owns a click in the
   viewport: a hot game widget should stop the editor picking under it, and an
   editor popup over the viewport should hide the pointer from the game UI.
   Belongs with `imguiruntime`'s input-capture item. Hit testing itself landed
   2026-09-28.
3. **Content from the game.** Replace `UIDemoScreen` as the only game UI with UI a game
   authors, through Lua or data.
4. **Text measurement**, for layout and for culling text by its bounds. A
   prompt or a button sized to its label guesses 8 pixels a character today.
5. **DPI.** Coordinates are framebuffer pixels, as the editor's viewport rect
   is; nothing scales with the monitor yet. Nine-slice corners are drawn at
   their texel size, so they would want a scale too.
6. **Repeat while held.** A held direction steps once. Menus usually repeat
   after a delay and then at a rate; that fits in `UINavStepper`, which would
   then need the frame's time.
7. **Focus scopes.** One focus slot for the whole UI. A modal over a menu
   needs to save and restore it, which is `SetFocus` by hand today.
8. **Prompt glyphs.** `UIPrompt` is a label; a button prompt drawn as the
   pad's own icon can now be a `UISprite`, and needs an atlas keyed by
   binding. A per-pad family, Xbox or PlayStation, needs the window to report
   which pad is connected.
9. **A drifting stick.** `InputSystem` counts a stick past 0.2 as gamepad
   input every frame, so one resting past that keeps the gamepad active and
   the mouse never hides the highlight.
10. **Id scopes.** ImGui's id stack, where a parent's id seeds its children's, so
   two panels can each hold a "close" button without naming them apart. Not
   needed until a widget is made of other widgets; the first composite widget
   settles whether a stack or an explicit parent argument reads better.
11. **Unloading textures.** A texture lives as long as its `UIContext`. A game
   that swaps whole screens of art will want to drop one, which means ImGui's
   `WantDestroy` and a wait of a swapchain's worth of frames before the
   `ImTextureData` can go.
12. **The descriptor pool.** The Vulkan backend makes one descriptor per
   texture from the pool `VulkanRenderer::InitImGuiVulkanBackend` sizes at 64,
   shared with the font atlas. More UI textures than that needs atlases, which
   `UISprite::mSource` already reads, or a larger pool.
13. **Where the safe area comes from.** Nothing sets the insets yet. A
   console's title-safe area, or an overscan setting in Project Settings or
   the game's options, would set them through `SetSafeAreaInsets`.
14. **More layout.** Equal shares, wrapping, grids and scrolling wait for a
   screen that needs them, as rows and columns waited for the demo screen.

## Changed

*2026-10-02* — **A screen can hold the game's input, and a widget can be
pointer-only.** Was: nothing in gameui could keep input from gameplay, and
every enabled widget could take focus. Now: `UIContext::HoldInput()` and
`HoldsInput()`, written to `InputState::SetGameInputHeld` at `EndFrame`;
`UIWidgetOptions::mPointerOnly`; `UINavInput::mPause` and
`UIContext::PausePressed()` for the new `UIPause` action. All default off, so
existing screens are unchanged. `App` runs the game UI every frame, drawing the
game shell, and `UIDemoScreen` only under `--ui-demo`. Because the first real
screen, the pause menu, has to take the game's input without the editor's
capture flags, and the HUD's pause button has to stay out of the stick's reach.

*2026-10-02* — **Layout helpers and a safe area.** Was: every screen placed
its rects by hand from constants, and the demo screen was pinned to the top
left whatever the area. Now: `gameui/UILayout.h` has `Inset`, `Anchor` and
`UIStack`. `UIContext` gains `Bounds`, `SafeArea`, `SetSafeAreaInsets` and
`SafeAreaInsets`; the insets default to zero, so the safe area is the whole
area until something sets them. `UIDemoScreen` is laid out by
`UIDemoScreen::Layout(safeArea)`. Because hand-placed rects stopped fitting once
the same screen had to read at 16:9, 21:9 and 4:3, and inside a TV's
title-safe area.

*2026-10-02* — **Textures draw, and `Image` takes a `UISprite`.** Was:
`Image(ui, id, rect, TextureHandle, tint, options)` and
`UIDrawList::Texture(rect, TextureHandle, tint)`, the backend drew every
texture as a flat tint, and gameui linked `renderer` publicly for
`TextureHandle`. Now: `Image(ui, id, rect, const UISprite &, tint, options)`,
`UIDrawList::Texture(rect, UITexture, tint, uv)` beside the new `Sprite`,
`UIDrawCommand::mTexture` is a `UITexture` with an `mUV`, and gameui links
`assets` instead of `renderer`. `UIStyle` gains a sprite per grey-box colour,
all null, so a screen that sets none draws as before. `UIContext` owns a
`UITextures` and is destroyed after `ImGuiRuntime::Shutdown`. Because a
renderer `TextureHandle` could reach ImGui only through a descriptor made by
hand, and ImGui 1.92 creates and frees a texture it is handed itself.

*2026-10-02* — **`App` draws the game UI only under `--ui-demo`, and
`DrawPlaceholderHud` is gone.** Was: three placeholder panels and two prompts
every run. Now: `UIDemoScreen`, every core widget, when `AppDesc::mUIDemo` is
set, and no game UI otherwise. `UIWidgetOptions` gains `mAdjustsX` and
`UIInteraction` gains `mAdjust`; both default off, so existing calls are
unchanged. `UIStyle` gains the grey-box widget colours. Because the core
widgets replace the placeholder, and a debug screen should not draw in every
run.

*2026-10-01* — **`UIContext::BeginFrame` takes the `InputMap`, and
`UIState::BeginFrame` whether the focus highlight is shown.** Was:
`UIContext::BeginFrame(area, InputState&, UIPointer)` and
`UIState::BeginFrame(area, pointer, nav = {})`. Now:
`UIContext::BeginFrame(area, InputState&, const InputMap&, UIPointer)` and
`UIState::BeginFrame(area, pointer, nav = {}, focusVisible = true)`.
`UIInteraction` gains `mShowFocus`; draw the highlight from it rather than
`mFocused`. Because `Prompt` names an action's binding, which only the map
holds, and `UIState` swallows the press that brings the highlight back.
`InputSystem` now also makes the mouse active when the cursor moves.

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
