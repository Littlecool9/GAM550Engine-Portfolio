# editor

The ImGui context and backend lifecycle, and the Slate editor shell — dockspace,
default panel layout, the File, Project and Debug menus. It is the only place in
the engine that calls `ImGui::*` or `ImGui_Impl*`.

`mir::editor` · depends `mir::renderer`, `mir::window` (public); `mir::core`, `mir::editortheme`, `imgui`, `iconfontcppheaders` (private) · API `modules/editor/include/editor/` · maintainer Sumin Park · reviewed 2026-09-17

## Model

`App` drives `Editor` through several calls a frame and reads state back.

```cpp
mEditor.BeginFrame();                                   // the ImGui NewFrame trio

if (windowVisible)
    Act(mEditor.DrawLayout(mDesc.mEnableEditorLayout,   // EditorActions
                           world.Resource<InputMap>(), world.Resource<RenderSettings>(),
                           world.Resource<EngineModeState>(), projectMenu, currentProject, world));

mRenderer.SetImGuiDrawData(mEditor.EndFrame());         // ImGui::Render, hand off draw data
mRenderSystem.SetViewportRect(0, mEditor.SceneViewportRect());

if (IsEditing(world))                                   // Play shows the World's own camera instead
{
    mEditor.UpdateCamera(dt);
    mRenderSystem.SetExternalCamera(0, mEditor.SceneCamera().Matrix(), mEditor.SceneCamera().mLens);
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

`EditorCamera` is a free-fly camera for the scene viewport — position plus
yaw/pitch, not a `WorldTransform` (that type's matrix is resolver-only,
unconstructable outside the ECS hierarchy resolve, and this camera is
deliberately outside the `World`: it must survive a Play session's
snapshot/restore untouched). `Update` reads `Window::RawInput()` directly
rather than `InputMap`/`InputState`, so a project's key bindings can't affect
the editor's own camera. Right-drag flies/looks (WASD/QE, scroll for speed);
`Editor::UpdateCamera` gates that on `!WantsCaptureMouse()` so a drag ImGui
itself is handling doesn't also move it.

## API

```cpp
bool Initialize(Window &window, VulkanRenderer &renderer, std::filesystem::path projectSettingsPath);
void Shutdown(VulkanRenderer &renderer);

void BeginFrame();
EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                         const EngineModeState &mode, std::span<const ProjectMenuEntry> projects,
                         std::string_view currentProject, World &world);
ImDrawData *EndFrame();

VkRect2D SceneViewportRect() const;
bool IsInitialized() const;

void UpdateCamera(float dt);          // once per frame, after BeginFrame/DrawLayout
EditorCamera &SceneCamera();
```

| Type | Header | Role |
|---|---|---|
| `SceneMenuAction` | `editor/Editor.h` | `None`/`New`/`Save`/`Load`/`Play`/`TogglePause`/`Stop` |
| `ProjectMenuEntry` | `editor/Editor.h` | `{mDir, mName}` for one game in Open Project |
| `EditorActions` | `editor/Editor.h` | `{mScene, mOpenProject}`, what the menus and toolbar asked for this frame |
| `EditorCamera` | `editor/EditorCamera.h` | Free-fly scene camera, position + yaw/pitch, outside the `World` |
| `Selection` | `editor/Selection.h` | `{mSelected}`, the `World` resource `Hierarchy` writes and `Inspector` reads |

`LogPanel`, in `src/editor/panels/LogPanel.cpp`, renders `core`'s `LogHistory`
into the `Output` dock. `HierarchyPanel`, in
`src/editor/panels/HierarchyPanel.cpp`, renders the `World`'s live entities
into the `Hierarchy` dock as a tree built from `HierarchyIndex` (roots are
entities `ParentOf` reports null for). `InspectorPanel`, in
`src/editor/panels/InspectorPanel.cpp`, renders the `Selection`'s components
into the `Inspector` dock. None of the three are public — panels are reached
only through `DrawLayout`.

Clicking an entity in `Hierarchy` writes it into `Selection`
(`editor/Selection.h`), a `World` resource `HierarchyPanel` installs on first
draw if nothing already has. `HierarchyPanel` clears it back to `kNullEntity`
whenever the selected entity is no longer alive, so a stale handle never
lingers past the frame its entity died. `InspectorPanel` reads it with
`TryResource` rather than `Resource`, so it degrades to "No entity selected"
instead of asserting on a frame where `Hierarchy` hasn't drawn yet (e.g. it
starts collapsed).

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
current value, greyed out via `ImGui::BeginDisabled`. No 3D gizmo and no
component add/remove — numeric/text controls over the existing field tables
only, per the roadmap's scope cut.

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

**Destruction order is load-bearing.** `App` declares `Editor` right after
`mRenderer` and before `mWorld`, so the ImGui Vulkan backend tears down before
`VulkanRenderer::Shutdown()` and the GLFW backend before `mWindow` dies.

**`~Editor()` is a no-op if `Shutdown()` was skipped.** It leaks the ImGui context
rather than reach into a `VulkanRenderer&` it was never given.

**`Editor` is a class rather than free functions**, because it carries state
across calls — the viewport rect, the Style Editor toggle, the game waiting on
the confirmation dialog, and whether it is initialised. As free functions those
would be out-parameters threaded through every call.

Fonts are Inter for body text and Font Awesome 6 for icons, merged into one atlas
and rescaled for the window's `ContentScale()`.

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
3. **No 3D gizmo for `Transform`.** `Inspector` edits `position`/`rotation`/
   `scale` as plain drag boxes; a viewport gizmo is its own issue (#59).
4. **Serialized custom layouts**, beyond whatever `imgui.ini` remembers.
5. **Input routing**, beyond what `ImGui_ImplGlfw` gives for free.
6. **Make `Editor` optional on `App`** — a `unique_ptr`, null in a packaged game
   build. This module's boundary is what makes that swap possible without
   touching `RenderSystem` or `VulkanRenderer`.
7. **A second viewport showing `EditorCamera` during Play+Paused**, so pausing
   lets you look around the live World independent of the game camera. Needs
   `RenderSystem::AddViewport` (see [renderer.md](renderer.md)) wired to a
   second panel; nothing calls it yet.

## Changed

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
