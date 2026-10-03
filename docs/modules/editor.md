# editor

The Slate editor shell — dockspace, default panel layout, the File, Project and
Debug menus — drawn into the ImGui context `imguiruntime` owns. It owns no part
of that context's lifetime: no `CreateContext`, no fonts, no backend. See
[imguiruntime.md](imguiruntime.md).

`mir::editor` · depends `mir::imguiruntime`, `mir::renderer`, `mir::window` (public); `mir::core`, `mir::editortheme`, `imgui`, `imguizmo`, `iconfontcppheaders` (private) · API `modules/editor/include/editor/` · maintainer Sumin Park · reviewed 2026-09-17

## Model

`App` drives `Editor` through several calls a frame and reads state back.

```cpp
mImGui.BeginFrame();                                    // ImGuiRuntime: the NewFrame trio

if (windowVisible)
    Act(mEditor.DrawLayout(mDesc.mEnableEditorLayout,   // EditorActions
                           world.Resource<InputMap>(), world.Resource<RenderSettings>(),
                           world.Resource<EngineModeState>(), projectMenu, currentProject, world));

mRenderer.SetImGuiDrawData(mImGui.EndFrame());          // ImGuiRuntime: ImGui::Render, hand off draw data
mRenderSystem.SetViewportRect(0, mEditor.SceneViewportRect());

if (IsEditing(world))                                   // Play shows the World's own camera instead
{
    mEditor.UpdateCamera(dt);
    mRenderSystem.SetExternalCamera(0, mEditor.SceneCamera().Pose(), mEditor.SceneCamera().mLens);
}
```

`DrawLayout` returns `EditorActions`: the `SceneMenuAction` the File menu or the
Play/Pause/Stop toolbar picked this frame, and the directory of a game picked
under Project > Open Project and confirmed. The editor decides what was asked for
and `App` decides what it means, so this module never touches `SceneIO`,
`PlaySnapshot` or a `GameProject` — `EngineModeState` is read-only here, just to
draw the toolbar's current label. `HierarchyPanel` does read the `World`'s
entities (see below), but only to display and select them; it never creates,
destroys or reparents one.

Project > Open Project lists the `ProjectMenuEntry` values `App` passes in — each
game's name, with its directory in the shortcut column — and ticks the one whose
directory is `currentProject`. Picking another opens a modal warning that the
editor restarts with that game and that unsaved scene changes will be lost. Only
its Open button puts the directory into `EditorActions::mOpenProject`; `App`
does the restart.

`DrawLayout(true)` builds the dockspace once. It splits into left, right and
bottom with `DockBuilderSplitNode`, keeps the centre undocked through
`ImGuiDockNodeFlags_PassthruCentralNode`, docks `Hierarchy`, `Inspector` and
`Output` into the splits, draws a Play/Pause/Stop toolbar strip and the menu bar,
and optionally the Style Editor and Project Settings windows.

Passing `false` skips all of that and resets `SceneViewportRect()` to zero
extent, which the renderer reads as "use the full swapchain". An ImGui context
still runs, so a caller can draw its own UI.

`EditorCamera` is a free-fly camera for the scene viewport — a double-precision
`WorldPosition` plus yaw/pitch, handed out as a `WorldPose`, not a `WorldTransform` (that type's matrix is resolver-only,
unconstructable outside the ECS hierarchy resolve, and this camera is
deliberately outside the `World`: it must survive a Play session's
snapshot/restore untouched). `Update` reads `Window::RawInput()` directly
rather than `InputMap`/`InputState`, so a project's key bindings can't affect
the editor's own camera. Right-drag flies/looks (WASD/QE, scroll for speed);
`Editor::UpdateCamera` gates that on `!WantsCaptureMouse()` so a drag ImGui
itself is handling doesn't also move it.

## API

```cpp
bool Initialize(const ImGuiRuntime &imgui, Window &window, std::filesystem::path projectSettingsPath,
                World &world);    // false if imgui is not initialized
void Shutdown();                  // before ImGuiRuntime::Shutdown

// between ImGuiRuntime::BeginFrame and EndFrame
EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                         const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                         std::string_view currentProject, World &world);

VkRect2D SceneViewportRect() const;
bool IsInitialized() const;

void UpdateCamera(float dt);          // once per frame, after DrawLayout
EditorCamera &SceneCamera();
```

| Type | Header | Role |
|---|---|---|
| `SceneMenuAction` | `editor/Editor.h` | `None`/`New`/`Save`/`Load`/`Play`/`TogglePause`/`Stop` |
| `ProjectMenuEntry` | `editor/Editor.h` | `{mDir, mName}` for one game in Open Project |
| `EditorActions` | `editor/Editor.h` | `{mScene, mOpenProject}`, what the menus and toolbar asked for this frame |
| `EditorCamera` | `editor/EditorCamera.h` | Free-fly scene camera, `WorldPosition` + yaw/pitch (`Pose()`, `Translate`, `FocusOn`), outside the `World` |
| `Selection` | `editor/Selection.h` | `{mSelected}` (a `vector<Entity>`, ordered by selection - `.back()` is primary), the `World` resource `Hierarchy` and viewport picking write and `Inspector`/the gizmo read |

`LogPanel`, in `src/editor/panels/LogPanel.cpp`, renders `core`'s `LogHistory`
into the `Output` dock. `HierarchyPanel`, in
`src/editor/panels/HierarchyPanel.cpp`, renders the `World`'s live entities
into the `Hierarchy` dock as a tree built from `HierarchyIndex` (roots are
entities `ParentOf` reports null for). `InspectorPanel`, in
`src/editor/panels/InspectorPanel.cpp`, renders the `Selection`'s components
into the `Inspector` dock. None of the three are public — panels are reached
only through `DrawLayout`.

Clicking an entity in `Hierarchy`, or clicking it directly in the scene
viewport (`Editor::UpdateViewportPicking`, a ray/AABB test against every
`(WorldTransform, MeshRenderer, MeshBounds)` entity - see
[renderer.md](renderer.md) for `MeshBounds`), writes it into `Selection`
(`editor/Selection.h`), a `World` resource `HierarchyPanel` installs on
first draw if nothing already has. Both call the same
`ApplyClick(Selection&, Entity, bool additive)`: a plain click replaces the
whole selection, `additive` (ctrl- or shift-click, `io.KeyCtrl ||
io.KeyShift` - either works, same effect) toggles the clicked entity's
membership instead, and a null `Entity` (clicked empty space) clears the
selection unless `additive`. `HierarchyPanel` prunes any no-longer-alive
entities out of `mSelected` every frame, so a stale handle never lingers
past the frame its entity died - this runs before viewport picking in
`DrawLayout`, so picking never has to prune again itself. `InspectorPanel`
reads it with `TryResource` rather than `Resource`, so it degrades to "No
entity selected" instead of asserting on a frame where `Hierarchy` hasn't
drawn yet (e.g. it starts collapsed); with more than one entity selected it
shows a plain "N objects selected" instead of drawing components. The
transform gizmo (`DrawViewportGizmo`) only ever manipulates
`Selection::Primary()` (the most recently selected entity) - there is no
multi-object gizmo, so the others stay selected but untouched by it.
`DrawSelectionHighlights` draws a wireframe box (from each entity's
`MeshBounds`, projected with the same `viewProj` `UpdateViewportPicking`
builds) over every selected entity, not just the primary - brighter/opaque
for the primary, dimmer for the rest - since the gizmo alone gives no
viewport-visible sign that a non-primary entity is still selected.

`InspectorPanel` walks every `seq` up to `kMaxComponentTypes`, asking
`ComponentRegistry::FindBySeq` for each one and skipping those the selection
doesn't `Has` — the same walk `SceneIO` (see [scene.md](scene.md)) uses to
save a scene, and why it treats a C++ and a script-declared runtime component
identically: both surface through the same `ComponentOps`/`FieldDesc` tables.
Each field reads into a 64-byte scratch buffer (large enough for a `Mat4`,
`FieldKind`'s biggest kind), an ImGui control drawn over the copy per
`FieldKind`, and `FieldDesc::Write` called on the live component only if the
control reports an edit — so a component with a custom accessor (`Transform`'s
`SetPosition` and friends) runs through it rather than being memcpy'd under
it. A `FieldDesc::ReadOnly` field (`WorldTransform.matrix`) still shows its
current value, greyed out via `ImGui::BeginDisabled`. No component
add/remove — numeric/text controls over the existing field tables only, per
the roadmap's scope cut. (A 3D transform gizmo does exist, drawn over the
viewport by `DrawViewportGizmo` via ImGuizmo - it's a separate,
`Selection`-driven overlay, not part of `InspectorPanel`'s field-table
drawing described here.)

## Rules

**`SceneViewportRect()` comes from `DockBuilderGetCentralNode`, never from a
named window.** The central node is the undocked centre where the swapchain shows
through, so it is the real viewport even though it has no window and no name.

Reading a window's content region instead is the bug this rule exists to prevent.
See Changed.

Anyone adding a genuine named viewport window has to update `DrawLayout`'s
source. There is no compiler or runtime check for it.

**The rect is only valid within or after `DrawLayout(true)` in the same frame.**

**The Open Project dialog is opened after the menu bar ends, not from the menu
item.** `OpenPopup` and `BeginPopupModal` have to run under the same ID stack.
Called from inside the menu, `OpenPopup` opens a popup that nothing ever draws.

**Initialize after `ImGuiRuntime`, shut down before it.** `Initialize` refuses an
uninitialised runtime. It turns on docking, applies the theme and its DPI scale,
and points `io.IniFilename` at `imgui.ini` next to the executable; ImGui reads
that file lazily on the first `NewFrame`, so setting it after `CreateContext`
still loads it.

**`Shutdown` saves `imgui.ini` and detaches it.** `DestroyContext` would otherwise
save through `IniFilename`, a pointer into this `Editor`'s string, after the
editor has shut down. It saves only if ImGui got as far as loading settings, the
same guard `DestroyContext` uses, so a failed startup never overwrites the file
with an empty one.

**`Editor` is a class rather than free functions**, because it carries state
across calls — the viewport rect, the Style Editor toggle, the game waiting on
the confirmation dialog, and whether it is initialised. As free functions those
would be out-parameters threaded through every call.

Fonts are Inter for body text and Font Awesome 6 for icons, merged into one atlas
by `imguiruntime`; `EditorTheme::ScaleForDpi` rescales them here for the window's
`ContentScale()`.

**The Play/Pause/Stop toolbar is another `Up` viewport side bar**, drawn right
after the title bar and before the dockspace, so it stacks the same way the
title bar already does — each `BeginViewportSideBar` call shrinks the main
viewport's remaining work area for whoever asks next.

**`EditorCamera` has no `Transform` component, deliberately.** It isn't a
`World` entity, so a `Transform` has nothing to attach to; `SetExternalCamera`
takes the camera's world matrix directly rather than a `WorldTransform`.

**The File menu's New/Save/Load are disabled during Play**
(`ImGui::BeginDisabled(mode.mMode == EngineMode::Play)`). This is the visible
half of the rule; `App` enforces it independently (see [app.md](app.md)) since
this module only greys out a button, it does not stop another caller from
reaching `App::SaveScene()` directly.

## State

*As of 2026-09-14.* One fixed layout: `Hierarchy` left, `Inspector` right,
`Output` bottom, a Play/Pause/Stop toolbar strip, and the scene showing through
the passthru centre. It is built once and left alone, persisted only through
ImGui's own `imgui.ini` next to the executable, which also carries it across an
Open Project restart.

`Output` is wired to `LogHistory` through `LogPanel`. Its history starts empty in
the process an Open Project restart launches.

`EditorCamera` flies and looks in Edit mode; `App` stops feeding it input and
switches viewport 0 to the World's default camera in Play (see
[app.md](app.md)).

No tests. It needs a device and a display. `smoke.switch` covers the restart that
Open Project asks for, but not the menu or the dialog themselves.

## Backlog

1. **`Inspector` has no way to add or remove a component.** It only edits the
   fields of what the selection already has; issue #38 scoped that out
   explicitly, same as the "no gizmo" cut below.
2. **`Hierarchy` shows no entity names.** No component carries a display
   name yet, so each row is just `Entity <index>`.
3. **No multi-object transform gizmo.** With more than one entity selected,
   `DrawViewportGizmo` only manipulates `Selection::Primary()` - moving
   several selected objects together as a group, preserving their relative
   offsets, needs a shared-pivot gizmo this doesn't have.
4. **Serialized custom layouts**, beyond whatever `imgui.ini` remembers.
5. **Input routing**, beyond what `ImGui_ImplGlfw` gives for free.
6. **Make `Editor` optional on `App`** — a `unique_ptr`, null in a packaged game
   build. This module's boundary is what makes that swap possible without
   touching `RenderSystem` or `VulkanRenderer`.
7. **A second viewport showing `EditorCamera` during Play+Paused**, so pausing
   lets you look around the live World independent of the game camera. Needs
   `RenderSystem::AddViewport` (see [renderer.md](renderer.md)) wired to a
   second panel; nothing calls it yet.
8. **Viewport picking only tests axis-aligned bounding boxes**, not actual
   mesh geometry - a click inside an object's box but over empty space
   between its triangles (e.g. near a corner) still selects it. No
   per-triangle picking, matching the renderer's own choice not to retain
   CPU-side mesh geometry after upload.

## Changed

*2026-10-02* — **Optional.** Was: built and linked into every binary. Now: not
built at all when `MJOLNIR_WITH_EDITOR` is OFF (the `windows-msvc-game-*`
presets), along with `editortheme` and ImGuizmo; `app` is the only module that
links it, and only with the option on. See [app.md](app.md#without-the-editor).

*2026-09-30* — **The scene camera has no far plane.** Was: `EditorCamera::mLens`
kept the default `farZ` of 100 m, so the scene view clipped everything beyond
it. Now: its `farZ` is 0, which the reverse-Z projection reads as no far plane
(see [renderer.md](renderer.md)). ImGuizmo still needs a finite one, so
`DrawViewportGizmo` gives it a plane a few times farther than the entity.

*2026-09-29* — **The scene camera, picking, highlights and gizmo became
large-world safe; F frames the selection.** Was: `EditorCamera::mPosition` was a
`glm::vec3` and `Matrix()` a float world matrix; picking, the selection
outlines and ImGuizmo all narrowed entities against the Scene origin, so at the
map edge everything snapped to float's 1/64 m grid. Now: the camera keeps a
`WorldPosition` and exposes `Pose()`; those three paths work in the scene
viewport's camera-relative space (`TryMatrixRelativeTo(camera.position)`,
`MakeCameraRay`, `RayIntersectsBox`). The gizmo writes back one channel per
operation: Translate turns `camera + offset` into a root position or, through
`TryWorldToParentLocal`, a child's parent-local translation; Rotate and Scale
never touch the position. `FocusSelection` (F, Edit only, not while typing)
moves the camera next to `Selection::Primary()` with `EditorCamera::FocusOn`.

*2026-09-26* — **The ImGui context's lifetime moved to `imguiruntime`.** Was:
`Initialize` created the context, loaded the fonts and initialised both
backends, `Shutdown(VulkanRenderer&)` tore them down, and `BeginFrame`/`EndFrame`
were the `NewFrame` trio and `Render`. Now: all of that is `ImGuiRuntime`'s,
`Initialize` takes the runtime instead of the renderer, `Shutdown` takes nothing,
and `BeginFrame`/`EndFrame` are gone. A game UI can draw into the same context
without an editor in the build.

*2026-09-21* — **Shift-click is now equivalent to ctrl-click, and every
selected entity gets a viewport outline, not just the primary.** Was:
`additive` was `io.KeyCtrl` alone, so shift-click silently behaved like a
plain replacing click; the only viewport-visible sign of selection was the
gizmo on `Selection::Primary()`, so ctrl-clicking a second entity looked
indistinguishable from replacing the selection unless you were watching the
Hierarchy panel. Now: `additive` is `io.KeyCtrl || io.KeyShift` in both
`Editor::UpdateViewportPicking` and `HierarchyPanel`, and a new
`Editor::DrawSelectionHighlights` draws a wireframe `MeshBounds` box over
every entity in `Selection::mSelected` each frame (before
`DrawViewportGizmo`, same `viewProj` `UpdateViewportPicking` builds, via
`ImGui::GetForegroundDrawList` the same way the gizmo already draws over
the scene).

*2026-09-21* — **Clicking an object in the scene viewport selects it, and
`Selection` supports more than one entity.** Was: the only way to select
anything was a `Hierarchy` row, and `Selection::mSelected` was a single
`Entity`. Now: `Editor::UpdateViewportPicking` runs each frame (after
`HierarchyPanel::Draw`, before `DrawViewportGizmo`, in `DrawLayout`),
edge-detecting a left click inside the scene viewport rect (guarded by
`!WantsCaptureMouse()` and `!ImGuizmo::IsOver()/IsUsing()`, so it never
fires over a panel or while dragging the gizmo), builds a world-space ray
from the click (`CameraMath.h`'s new `MakeCameraRay`, using the same
`viewProj` convention `MakeViewProjection` already produces), and finds the
nearest entity whose `MeshBounds` (new component, see
[renderer.md](renderer.md)) it hits in local space
(`RayIntersectsAABB`). `Selection::mSelected` is now a `vector<Entity>`;
`ApplyClick` (`editor/Selection.h`) is the one place the
replace/toggle/deselect rule lives, shared by `HierarchyPanel` and viewport
picking so ctrl-click behaves identically from either entry point.
`InspectorPanel` shows "N objects selected" for more than one;
`DrawViewportGizmo` manipulates only `Selection::Primary()`.

*2026-09-17* — **`Inspector` populated from the Selection.** Was: an empty
`ImGui::Begin`/`End`, same as `Hierarchy` before it. Now: `InspectorPanel`
reads `Selection` (see above) and, for each component the selected entity
`Has`, draws its `FieldDesc` table as ImGui controls — `DragFloat`/`Vec3`/
`Vec4`/etc. per `FieldKind`, a read-only field shown greyed out rather than
skipped, edits written back through `FieldDesc::Write` so a component with an
accessor (`Transform`) runs its setter instead of being overwritten byte for
byte. Walking `ComponentRegistry::FindBySeq` rather than something typed to
C++ components is what makes a script-declared runtime component show up the
same way, with no separate code path.

*2026-09-17* — **`Hierarchy` populated from the World.** Was: `Hierarchy` and
`Inspector` were both `ImGui::Begin`/`End` with nothing drawn between them.
Now: `DrawLayout` takes a `World&`, and `HierarchyPanel` walks it every frame
— roots are entities `HierarchyIndex::ParentOf` reports null for, children
come from `ChildrenOf`, so the tree is read fresh each frame rather than
cached, which is what makes it track entities a running scene creates or
destroys without any extra invalidation. Clicking a row writes the entity into
the new `Selection` resource; `Inspector` (issue #38) reads it from there.
`Inspector` itself is still an empty window.

*2026-09-17* — **Project Settings gained a Rendering section.** Was:
`ProjectSettingsPanel` drew only the Input section (actions/bindings), and its
own "Save" wrote `InputMap` to `project_settings.json`. Now: it also takes a
`RenderSettings&` and draws a Rendering section below Input — a ray tracing
checkbox today, laid out as one row per setting so the next one is a row, not
a rewrite — with its own "Save" writing `RenderSettings` (see
[renderer.md](renderer.md)) to the same file. Each section's `SaveFile` reads
the file first and only replaces its own top-level key, so saving one section
never touches the other's. `DrawLayout` threads the `RenderSettings&` through
to `ProjectSettingsPanel::Draw` the same way `InputMap&` already was.

*2026-09-14* — **Project > Open Project.** Was: `DrawLayout` returned a bare
`SceneMenuAction`, and the Project menu held only Project Settings. Now: it
returns `EditorActions`, which adds a game directory picked from the Open Project
submenu and confirmed in a modal. `App` passes the list in and restarts into the
picked game, so this module still knows nothing about games.

*2026-09-12* — **File menu greyed out during Play.** Was: New/Save/Load stayed
clickable regardless of mode, and `App` (see [app.md](app.md)) silently
no-oped them with a log warning. Now: `ImGui::BeginDisabled` around the three
items while `mode.mMode == EngineMode::Play`, so the UI itself shows why
nothing happens rather than relying on the log.

*2026-09-12* — **`EditorCamera` and a Play/Pause/Stop toolbar added.** Was:
`DrawLayout` took only `enableLayout`, and the scene viewport always showed
whatever World `Camera` `RenderSystem` found first. Now: `DrawLayout` also
takes `InputMap`/`EngineModeState`, draws a toolbar that reads/writes Play mode
through `SceneMenuAction`, and `Editor` owns a free-fly `EditorCamera` that
`App` feeds into viewport 0 while editing.

Driven by wanting the scene view navigable independent of any World camera,
and by Play/Pause needing a visible way to start and stop, not just the
`EngineModeState` resource existing.

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
