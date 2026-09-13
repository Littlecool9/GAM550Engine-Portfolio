# app

The composition root and the frame loop. `App` is the one place that knows every
other module exists. It contains no gameplay and no subsystem of its own, and it
is not a framework — there is no registry, no service locator, and no virtual
`Game` to subclass.

`mir::app` · depends `mir::core`, `assets`, `window`, `renderer`, `editor`, `scene`, `script`, all public; `nlohmann_json` (private) · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-09-12

## Model

Fixed members and three calls: `Initialize(desc)`, then `Run()`, then
`Shutdown()`.

The member list is the initialisation order, and reverse-order destruction is
what makes teardown correct, so declaration order in `App.h` is load-bearing
rather than cosmetic. Adding a subsystem means editing `App`, which forces a
decision about where it belongs in that order.

`Initialize` resolves the project before anything else — the window needs a
title before it exists. `AppDesc::mProjectPath` names a `game.json`; a private
`GameProject`/`LoadGameProject` in `App.cpp` parses it into a window title and
two private members, `mSceneDir` and `mAssetsRoot`, that `SaveScene`,
`LoadScene` and the resolve passes below all read. A missing or malformed
`game.json` falls back to `GameProject{}`'s defaults, logged, rather than
failing `Initialize`.

`Run` is a plain loop with no fixed timestep.

```
Start(scheduler)                       systems cache their queries
while not ShouldClose:
    PollEvents                         close flag and size refresh here
    dt = min(elapsed, mMaxDeltaSeconds)
    Editor::BeginFrame
    Editor::DrawLayout -> SceneMenuAction    New/Save/Load/Play/TogglePause/Stop acted on here
    renderer.SetImGuiDrawData(Editor::EndFrame())
    renderSystem.SetViewportRect(0, Editor::SceneViewportRect())
    if IsEditing(world):                     viewport 0's camera, only in Edit
        Editor::UpdateCamera(dt)
        renderSystem.SetExternalCamera(0, Editor::SceneCamera())
    else:
        renderSystem.SetViewportCamera(0, null)   falls back to the World's own Camera
    ScriptReloadWatcher::Poll
    SystemScheduler::Update            PreUpdate, Update, PostUpdate, Render
    ++frame
```

`App` contains no ImGui call and never builds a `DrawItem`. The editor produces a
draw-data pointer and a rect, and `RenderSystem` does the drawing from the
`Render` phase.

A `SystemContext` is rebuilt every tick rather than stored. It bundles references
to the world and command buffer with the values that change each frame, so
keeping one around would only be a stale copy.

```cpp
mir::InitLog();                 // outside App: early failures must still log

mir::App app;

mir::AppDesc desc{};
desc.mProjectPath = "games/HelloWorld/game.json";

if (!app.Initialize(desc))
{
    mir::FlushLog();
    return -1;
}

app.Run();
app.Shutdown();                 // idempotent; the destructor would do it too
mir::FlushLog();
```

Systems are registered on `app.Systems()` between `Initialize` and `Run`.

### Play, Pause, Stop

`EngineModeState` (see [core.md](core.md)) is a `World` resource; `App` is the
only thing that flips it. `PlayScene`/`StopScene`/`TogglePause` are the three
transitions, wired to the editor toolbar through `SceneMenuAction` today.

```cpp
PlayScene()    // Edit -> Play. Captures the World first (PlaySnapshot).
StopScene()    // Play -> Edit. Stops every started script, then restores the capture.
TogglePause()  // Running <-> Paused. No-op outside Play.
```

`StopScene` calls `ScriptSystem::StopAllScripts` (giving every started script
its `OnStop`, a Unity-style cleanup hook) *before* calling
`PlaySnapshot::RestoreWorld`, since the World those scripts ran in is about to
be destroyed wholesale. It then resolves scripts and meshes again, same as
`LoadScene` — `RestoreWorld`'s entities come from `LoadScene` under the hood,
with the same only-names-round-trip gap.

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size, Vulkan app name and validation, editor-layout gate, `dt` clamp, project path |
| `App` | `app/App.h` | Owns the subsystems, runs the loop |

`AppDesc::mProjectPath` replaces what used to be separately caller-set window
title/scene-directory/assets-root fields; see Model. `mAppName` is the one
project-shaped-looking field that stays caller-set — it is the Vulkan instance
name, engine-level, and nothing user-facing ever sees it.

Accessors: `GetWorld()`, `Systems()`, `Scripts()`, `ScriptReload()`, `Splash()`,
`Assets()` and `Scene()`. `Renderer()` is marked a temporary seam, because
`Renderer.h` is still an empty abstraction placeholder.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds. `LoadScriptAsset(assetPath,
scriptName)` is public because game bootstrap code needs it too, not just the
internal resolve passes.

### Ownership and order

| Member | Created | Destroyed | Because |
|---|---|---|---|
| `mSplash` | first | last | shown before the window exists |
| `mWindow` | early | late | the renderer's surface is built from it |
| `mRenderer` | after the window | before it | holds `VkSurfaceKHR` and the device |
| `mEditor` | after the renderer | before it | its Vulkan backend needs a live device, its GLFW backend a live window |
| `mCommands` | before `mWorld` | after it | outlives the world it records against |
| `mWorld`, `mScheduler`, `mScriptHost`, `mScene` | with `App` | with `App` | no external resources |
| `mAssetManifest` | first `Assets()` | after the cache | owns the parsed manifest |
| `mAssetCache` | with the manifest | before it | holds a raw pointer into it |

The manifest and cache pair is the one real aliasing hazard, and it is handled
twice. Declaration order makes implicit destruction correct, and `Shutdown` also
calls `reset()` on them explicitly in the same order, because `Initialize` may
follow.

`mRenderSystem`/`mScriptSystem` are non-owning — `AddSystem<T>` returns a `T&`
into `mScheduler`'s storage, captured here so `Run` and `StopScene` can reach
them without a lookup.

### Resolving names back into handles

A `LoadedScene` file (see [scene.md](scene.md)) round-trips a `MeshRenderer`'s
`meshName`/`materialShader` and a `ScriptRef`'s `scriptName` — strings, not the
live `MeshHandle`/`instanceRef` `RenderSystem`/`ScriptSystem` actually need.
`ResolveSceneMeshes`/`ResolveSceneScripts`, private to `App`, turn one into the
other by asset id, and run at the end of both places entities come from a
`LoadedScene` file: `LoadScene()` and `StopScene()` (via `PlaySnapshot::
RestoreWorld`). Neither call site can skip them independently — both live
inside the same two methods.

This lives in `App`, not `main.cpp`, because it needs `Assets()`/`Renderer()`/
`Scripts()`, all of which `App` already owns, and `mAssetsRoot` is config data
of the same shape as `mSceneDir` — not a policy that differs per game.

## Rules

**Register systems before `Run`.** `Run` calls `SystemScheduler::Start`, and a
system added after that never gets `OnStart`.

**`Initialize` comes before anything else.** `Run` and `Shutdown` return
immediately and silently when it has not run, so a caller that ignores the `bool`
gets a process that exits successfully having done nothing.

**`Initialize` cleans up after itself.** A renderer failure resets the window
before returning `false`, so a failed `Initialize` leaves no half-built state.

**`Shutdown` is idempotent, and the destructor calls it.** Calling it explicitly
is the documented style, and forgetting it is not a leak.

**Systems stop before subsystems die.** `Shutdown` runs `Stop` first, so `OnStop`
still sees a live world. The renderer and window are already on their way out by
then, so touching them from `OnStop` is not supported.

**`Assets()` may return `nullptr`, and the failure is sticky.** With no manifest,
the disk is not re-checked every frame. `Shutdown` clears the flag so a later
`Initialize` retries.

**`dt` is clamped, and `mElapsed` accumulates the clamped value.** After a
breakpoint or a window drag it is behind the wall clock on purpose, because it is
simulated time rather than a timestamp and must not be used as one.

The clock is `steady_clock`, so a system clock change cannot produce a negative
`dt`.

**`App` does not own logging.** `InitLog` and `FlushLog` belong to the caller, so
a construction failure inside `App` is still visible.

**`App` is neither copyable nor movable**, and it hands out references to its own
members through `SystemContext`.

`MakeContext(0.0f)` is used for both `Start` and `Stop`, since neither is a frame
and a nonzero `dt` there would be a lie. `mFrame` increments after the update, so
the first tick runs as frame 0.

The `if (!mWindow)` branch after `Window::Create` is currently unreachable,
because the GLFW backend aborts through `MIR_CHECK` instead of returning null.
See [window.md](window.md).

**`PlayScene`/`StopScene`/`TogglePause` are the only way `EngineModeState`
changes.** `StopScene` calls `StopAllScripts` before `RestoreWorld` runs,
never after — the World those scripts ran in still has to exist for
`CallOnStop` to mean anything, and `RestoreWorld` destroys it wholesale a
moment later.

**Viewport 0's camera is mode-gated, not just fed unconditionally.**
`Editor::UpdateCamera` only runs in Edit. Feeding it in Play would let a
right-drag meant to look around the game view — viewport 0 shows the World's
camera then, not the editor one — silently fly the (invisible) editor camera
off somewhere, surfacing only later when Stop reverts viewport 0 to wherever
that ended up.

**`mSceneDir`/`mAssetsRoot` are private, not `AppDesc` fields.** They are
derived from `mDesc.mProjectPath`, not caller config — a project switch would
need to re-resolve both together, so nothing outside `Initialize` should set
one without the other.

**`NewScene`/`SaveScene`/`LoadScene` refuse while Play is running.** These can corrupt the current scene. Both `Editor` and app will opt out.

## State

*As of 2026-09-12.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, and the scene renders.
Play/Pause/Stop work end to end: a Play session can be entered, its World
mutations discarded on Stop via `PlaySnapshot`, and resumed indefinitely.

`main.cpp` is down to setting `AppDesc::mProjectPath` and calling
`LoadScene()` — `BuildScene` (an in-code fallback scene) is gone, since
`games/HelloWorld/scenes/default/scene.json` is checked into the repo and
`LoadScene()` always succeeds on a fresh clone.

The renderer is a value member rather than a `unique_ptr`, because there is no
interface to hold.

No tests. `App` is composition, and its behaviour is a window opening, a device
initialising and a loop running. The parts worth asserting — phase ordering,
command buffer flushing, asset lookup, scene round-tripping — are tested where
they live. A headless renderer is the prerequisite for testing anything here.

## Backlog

1. **A headless mode**, which needs the renderer abstraction. It is also the
   prerequisite for any test in this module, and for making `Editor` optional.
2. **No fixed timestep**, so physics-style systems have no stable substep.
   Undecided whether that belongs in the scheduler or in the loop.
3. **No frame limiter**, and no window-event handling beyond the renderer
   re-querying size.
4. **`Initialize` after `Shutdown`** is written to work — the sticky asset flag
   and the explicit resets exist for it — but nothing exercises it.
5. One `App` per process is assumed everywhere, and nothing enforces it.
6. **No project switching yet.** `AppDesc::mProjectPath` and the private
   `GameProject` resolution set up for it (see Model), but nothing calls the
   resolution a second time after `Initialize`.
7. **A second viewport for Play+Paused**, showing the editor camera alongside
   the game view. `RenderSystem::AddViewport` exists (see
   [renderer.md](renderer.md)); nothing calls it yet.
8. **No World diffing for Play/Stop** — `PlaySnapshot` destroys and reloads
   every entity rather than diffing what changed, which is simple but not
   cheap for a large scene.

## Changed

*2026-09-12* — **`NewScene`/`SaveScene`/`LoadScene` refuse while Play is
running.** Was: nothing stopped any of the three from running mid-Play — Save
would overwrite the authored scene file with Play's live mutations, and
New/Load would desync `mPlaySnapshot` from the World it was captured from, so
a later Stop silently restored stale content instead of erroring. Now: each
checks `IsPlaying(mWorld)` first and no-ops with a logged warning; `Editor`'s
File menu also greys the same three items out for the same reason (see
[editor.md](editor.md)).

*2026-09-12* — **`AppDesc` takes a project path instead of pre-resolved
fields.** Was: `main.cpp` parsed `game.json` into a `GameProject` and copied
three fields (`mTitle`, `mSceneDir`, `mAssetsRoot`) into `AppDesc` by hand.
Now: `AppDesc::mProjectPath` names the `game.json`, and `App::Initialize`
resolves it internally into a private `GameProject` (moved from `main.cpp`)
and two private members. `main.cpp` shrank to setting one path.

Manual field-copying at the call site is exactly the shape of thing that
silently drifts — a second executable would have to re-derive the same three
fields correctly by hand, with nothing enforcing it stayed in sync. It also
sets up project switching: a future `SwitchProject(path)` re-runs the same
internal resolution `Initialize` does, rather than requiring the switcher to
re-derive and repackage fields externally.

*2026-09-12* — **`ResolveSceneMeshes`/`ResolveSceneScripts` moved from
`main.cpp` into `App`.** Was: `main.cpp` called them once, by hand, right
after its one `LoadScene()` call — so when `PlaySnapshot::RestoreWorld`
became a second caller of the same load path, it silently inherited the load
but not the resolve, and Stop left the viewport blank (null mesh handles,
unresolved script instances).

Neither function is policy that varies per game — same mechanism, every game,
parameterized by `mAssetsRoot` — so they moved to where `LoadScene()` and
`RestoreWorld()` both already live, closing the gap structurally: nothing can
call the load path without the resolve step now, because they're the same
call.

*2026-09-12* — **Play, Pause and Stop added.** `PlayScene`/`StopScene`/
`TogglePause` flip `EngineModeState` (see [core.md](core.md)), with
`PlaySnapshot` (see [scene.md](scene.md)) capturing the World before Play and
restoring it on Stop, and `ScriptSystem::StopAllScripts` giving every started
script its `OnStop` first. Wired to a toolbar in `Editor` (see
[editor.md](editor.md)).

*2026-09-12* — **Scene-viewport wiring moved from the renderer to
`RenderSystem`.** Was: `App::Run` called `renderer.SetSceneViewport(...)`
directly. Now: `mRenderSystem->SetViewportRect(0, ...)`, since `RenderSystem`
owns the viewport list (see [renderer.md](renderer.md#changed)). `App` also now
picks that same viewport's camera each frame — the editor's free-fly camera in
Edit mode, the World's default `Camera` in Play.

*2026-09-03* — **Drawing left `App::Run`.** Was: `CollectDrawInstances` and
`DrawFrame` called by hand between scheduler phases, so draw-list assembly had no
defined ordering against the systems that produce what it draws. Now:
`RenderSystem` in `SystemPhase::Render`, taking the renderer by constructor
reference. `App::Run` became poll, edit UI, poll reload, update.

*2026-09-03* — **ImGui left `App.cpp`.** Was: about 150 lines of `ImGui::*` and
`ImGui_Impl*` calls next to window and ECS orchestration, with `App.h` needing
dockspace internals just to declare a member. Now: an `Editor` member and four
calls, and `App.cpp` includes no ImGui header.
