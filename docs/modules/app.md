# app

The composition root and the frame loop. `App` is the one place that knows every
other module exists. It contains no gameplay and no subsystem of its own, and it
is not a framework — there is no registry, no service locator, and no virtual
`Game` to subclass.

`mir::app` · depends `mir::core`, `assets`, `window`, `renderer`, `editor`, `imguiruntime`, `scene`, `script`, all public; `input`, `audio`, `physics`, `serial`, `terrain` private · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

Fixed members and three calls: `Initialize(desc)`, then `Run()`, then
`Shutdown()`.

The member list is the initialisation order, and reverse-order destruction is
what makes teardown correct, so declaration order in `App.h` is load-bearing
rather than cosmetic. Adding a subsystem means editing `App`, which forces a
decision about where it belongs in that order.

`Run` is a plain loop with no fixed timestep.

```
Start(scheduler)                       systems cache their queries
while not ShouldClose:
    PollEvents                         close flag and size refresh here
    dt = min(elapsed, mMaxDeltaSeconds)
    ImGuiRuntime::BeginFrame                 the NewFrame trio
    Editor::DrawLayout -> EditorActions      New/Save/Load/Play/TogglePause/Stop and Open Project acted on here
    renderer.SetImGuiDrawData(ImGuiRuntime::EndFrame())
    renderSystem.SetViewportRect(0, Editor::SceneViewportRect())
    if IsEditing(world):                     viewport 0's camera, only in Edit
        Editor::UpdateCamera(dt)
        renderSystem.SetExternalCamera(0, Editor::SceneCamera())
    else:
        renderSystem.SetViewportCamera(0, null)   falls back to the World's own Camera
    ScriptReloadWatcher::Poll
    fixedSteps = FixedTimestep::Advance(dt)  every mode; FixedUpdate systems check IsSimulating
    SystemScheduler::Update            PreUpdate, Update, FixedUpdate x fixedSteps, PostUpdate, Render
    if ToggleFullscreen just pressed: ToggleFullscreen    F11; the swapchain follows next frame
    ++frame
    halfway through mMaxFrames, if mSwitchProjectTo: RequestOpenProject
    if mMaxFrames reached: RequestClose
```

`App` contains no ImGui call and never builds a `DrawItem`. `ImGuiRuntime`
produces a draw-data pointer and the editor a rect, and `RenderSystem` does the drawing from the
`Render` phase.

A `SystemContext` is rebuilt every tick rather than stored. It bundles references
to the world and command buffer with the values that change each frame, so
keeping one around would only be a stale copy.

`App` has one game open for its whole life, a `GameProject` from
`app/GameProject.h`: the game's directory and name, its window title, the asset
id root its scene names resolve under, its scene directory and its
`project_settings.json`. `Initialize` opens `AppDesc::mProject` and loads its
scene.

Switching games is a restart. `RequestOpenProject(dir)` only checks the directory
and asks the window to close; `main.cpp` relaunches the executable with
`--game <dir>` once `Run` and `Shutdown` have returned.

```
Project > Open Project, confirmed -> App::RequestOpenProject(dir)
    NormalizeGameDir, LoadGameProject      not a game: logged, keep running
    mProjectToOpen = game, Window::RequestClose
Run returns, Shutdown                      imgui.ini written, device and window gone
main.cpp: ShutdownLog, LaunchProcess(ExecutablePath(), --game dir)
next process: the ordinary startup path
```

`LoadScene` runs the two resolve passes in `src/app/SceneResolve.cpp` after
`SceneIO` loads, which is what turns saved script and mesh names back into live
instances and GPU handles. They are private to this module: the unload, release,
load, resolve order only holds while `App` is the one calling them.

```cpp
mir::InitLog();                 // outside App: early failures must still log

std::optional<mir::GameProject> project = mir::ResolveGameProject("games/HelloWorld");
if (!project)
    return 1;

mir::App app;

mir::AppDesc desc{};
desc.mProject = *project;

if (!app.Initialize(desc))      // loads the game's scene too
{
    mir::FlushLog();
    return -1;
}

app.Run();
app.Shutdown();                 // idempotent; the destructor would do it too

// app.ProjectToOpen() set: relaunch with --game, as main.cpp does
mir::FlushLog();
```

Systems are registered on `app.Systems()` between `Initialize` and `Run`.

### Play, Pause, Stop

`EngineModeState` (see [core.md](core.md)) is a `World` resource; `App` is the
only thing that flips it. `PlayScene`/`StopScene`/`TogglePause` are the three
transitions, wired to the editor toolbar through `SceneMenuAction` and to the
pause menu through `GameShellAction` (see [gameshell.md](gameshell.md)). The
menu's Pause and Resume pause and resume the simulation, Restart is Stop then
Play, and Quit stops Play in the editor and closes the window without it.
Without the editor there is no Play button, so `Initialize` calls `PlayScene`
when `mEnableEditorLayout` is off: a game build plays.

### Without the editor

`app` builds with or without the editor, by the `MJOLNIR_WITH_EDITOR` CMake
option (see [build.md](../build.md#a-build-without-the-editor)). It reaches the
code as `MIR_WITH_EDITOR`, 1 or 0, a PUBLIC compile definition of `mir::app`,
and `App::kHasEditor` says which build this is. Without it, `App` holds no
`Editor`, no `AssetImportManager` and no project menu; `mEnableEditorLayout` is
forced off; the scene viewport is the whole window through the World's camera;
and nothing sets `InputState::SetUiCapture`, since there is no editor UI to
want the input. Everything else, the game UI included, is the same code.

```cpp
PlayScene()    // Edit -> Play. Captures the World first (PlaySnapshot).
StopScene()    // Play -> Edit. Stops every started script, then restores the capture.
TogglePause()  // Running <-> Paused. No-op outside Play.
```

`StopScene` calls `ScriptSystem::StopAllScripts` (giving every started script
its `OnStop`, a Unity-style cleanup hook) *before* calling
`PlaySnapshot::RestoreWorld`, since the World those scripts ran in is about to
be destroyed wholesale. It then releases the Play session's GPU resources and
resolves scripts and meshes again, same as `LoadScene` — `RestoreWorld`'s entities come from `LoadScene` under the hood,
with the same only-names-round-trip gap.

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size, Vulkan app name and validation, editor-layout gate, `dt` clamp, frame limit, the game to open, a scripted game switch for tests |
| `App` | `app/App.h` | Owns the subsystems, runs the loop, takes a request to open another game |
| `GameProject` | `app/GameProject.h` | `{mDir, mName, mTitle, mAssetIdRoot, mSceneDir, mProjectSettingsPath}` for one game directory |

`app/GameProject.h` also declares `ReadDefaultGame`, `NormalizeGameDir`,
`LoadGameProject`, `ResolveGameProject` (normalize, then load) and
`DiscoverGameProjects` (every `games/*/game.json` next to the executable).

`mAppName` stays caller-set even though each game has its own title — it is the
Vulkan instance name, engine-level, and nothing user-facing ever sees it.

Accessors: `GetWorld()`, `Systems()`, `Scripts()`, `ScriptReload()`, `Splash()`,
`Assets()`, `Scene()`, `Project()`, `AvailableProjects()` and `ProjectToOpen()`.
`Renderer()` is marked a temporary seam, because `Renderer.h` is still an empty
abstraction placeholder.

`ToggleFullscreen()` switches the window between windowed and fullscreen and
asks the renderer to recreate its swapchain at the start of the next frame. `Run`
calls it when the `ToggleFullscreen` action is just pressed, F11 by default (see
`input/DefaultInputMap.h`). The recreate is requested rather than left to the
renderer's size check, because a window already the size of its monitor goes
fullscreen without changing size. The action is read after `Update`, so the frame
that saw the key renders at the old size and the next one at the new.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds. `NewScene`, `LoadScene` and
`StopScene` release the discarded entities' GPU meshes and materials. Game
control is `RequestOpenProject(dir)`, and the switch itself is a restart.

### Ownership and order

| Member | Created | Destroyed | Because |
|---|---|---|---|
| `mSplash` | first | last | shown before the window exists |
| `mWindow` | early | late | the renderer's surface is built from it |
| `mRenderer` | after the window | before it | holds `VkSurfaceKHR` and the device |
| `mEditor` | after the renderer | before it | its Vulkan backend needs a live device, its GLFW backend a live window |
| `mCommands` | before `mWorld` | after it | outlives the world it records against |
| `mWorld`, `mScheduler`, `mScriptHost`, `mScene` | with `App` | with `App` | no external resources |
| `mProject`, `mAvailableProjects`, `mProjectToOpen` | with `App` | with `App` | plain data |
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
`ResolveSceneMeshes`/`ResolveSceneScripts` turn one into the other by asset id
under the game's `mAssetIdRoot`, and run at the end of both places entities come
from a `LoadedScene` file: `LoadScene()` and `StopScene()` (via `PlaySnapshot::
RestoreWorld`). Neither call site can skip them independently — both live
inside the same two methods. A `materialShader` whose `CreateMaterial` fails
logs an error and resolves to the renderer's error material. The mesh then
draws magenta, rather than the default grey a null handle would give it.

### Terrain draws (private glue, no caller yet)

`src/app/TerrainRender.{h,cpp}` is the only place terrain meets the renderer,
since neither module may link the other. `TerrainRenderTiles::Add` takes a
`TerrainTileMesh` (see [terrain.md](terrain.md)), converts it with
`MakeTerrainVertices`, uploads it (with a BLAS on an RT device) and records a
`WorldDrawItem` at placement plus tile origin, added in double, with the
diagnostic tint on the default material and a `translationGrid` of 1/4096 m.
All tiles in one `TerrainRenderTiles` must come from one release; a mesh from
another is refused. `PublishTerrainDraws` copies the draws into the
`TerrainDrawItems` resource (see [renderer.md](renderer.md)); the project's
ray tracing setting decides how they are drawn. The `TerrainInstance` work
(#118) will call it from scene load.

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

**Switching games restarts the process.** The component registry, the counter
behind every component's signature bit, the Lua VM and the asset cache are all
process-wide. Only a new process opens a game without the last one's leftovers:
script-spawned entities, Lua globals, `world:declare`d component types.
`RequestOpenProject` tears nothing down itself.

**`RequestOpenProject` checks before it closes.** A directory that is not a game
logs an error and returns `false`, and the open game keeps running. It does not
use `ResolveGameProject`, whose "Loading game" line would claim a load that
happens in the next process.

**Unloading a scene does not call `OnStop`.** New and Load destroy the scene's
entities, and `ScriptSystem`'s destroy hook only drops each Lua instance.

**The game list is read once.** `DiscoverGameProjects` runs in `Initialize`. A
game added while the engine runs would still need a build to cook its assets, and
the asset manifest is never re-read.

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

**Only `App.h` and `App.cpp` branch on `MIR_WITH_EDITOR`.** Every
editor call goes in a `#if MIR_WITH_EDITOR` block there, the editor's
per-frame work in `ApplyEditorActions`, so the game build differs from the
editor's `--no-editor` path by what is missing and nothing else. The definition
is PUBLIC because `App.h` lays out `App` differently with it: a file that saw
the other value would disagree with `App.cpp` about the class.

**`NewScene`/`SaveScene`/`LoadScene` refuse while Play is running.** These can corrupt the current scene. Both `Editor` and app will opt out.

**Terrain meshes do not survive `ReleaseSceneResources`.** It frees every mesh
without bumping generations, so a kept handle would name the next scene's
mesh. Whoever owns a `TerrainRenderTiles` publishes empty terrain draws and
calls `Forget` around every release, then rebuilds from the same release;
`Destroy` is for dropping terrain while the scene stays.

## State

*As of 2026-09-14.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, Project > Open Project restarts
into another game, and the scene renders. Play/Pause/Stop work end to end: a
Play session can be entered, its World mutations discarded on Stop via
`PlaySnapshot`, and resumed indefinitely.

The renderer is a value member rather than a `unique_ptr`, because there is no
interface to hold.

No unit tests. `App` is composition, and its behaviour is a window opening, a
device initialising and a loop running. The parts worth asserting — phase
ordering, command buffer flushing, asset lookup, scene round-tripping — are
tested where they live. A headless renderer is the prerequisite for unit testing
anything here. Until then the `smoke.<game>` tests run the whole `App` for 120
frames per game through `mMaxFrames`, and `smoke.switch` restarts from one game
into another twice through `mSwitchProjectTo`. All of them fail on a crash or a
logged error.

## Backlog

1. **A headless mode**, which needs the renderer abstraction. It is also the
   prerequisite for any test in this module, and for making `Editor` optional.
2. **No frame limiter**, and no window-event handling beyond the renderer
   re-querying size.
3. **`Initialize` after `Shutdown`** is written to work — the sticky asset flag
   and the explicit resets exist for it — but nothing exercises it.
4. One `App` per process is assumed everywhere, and nothing enforces it.
5. **A second viewport for Play+Paused**, showing the editor camera alongside
   the game view. `RenderSystem::AddViewport` exists (see
   [renderer.md](renderer.md)); nothing calls it yet.
6. **No World diffing for Play/Stop** — `PlaySnapshot` destroys and reloads
   every entity rather than diffing what changed, which is simple but not
   cheap for a large scene.

## Changed

*2026-10-08* — **A failed `materialShader` resolves to the error material.**
Was: `ResolveSceneMeshes` stored the null handle from a failed
`CreateMaterial`, so the mesh drew with the default grey material. Now: it logs
an error and stores the renderer's `ErrorMaterial()`, so the mesh draws
magenta. Because: issue #19, where a null handle came to mean "nothing
authored".

*2026-10-05* — **Fullscreen toggle.** Was: the window's mode was fixed at
creation. Now: `App::ToggleFullscreen`, called from `Run` on the
`ToggleFullscreen` action (F11), toggles the window (see
[window.md](window.md)) and calls `VulkanRenderer::RequestSwapchainRecreate`.
Because: issue #44.

*2026-10-05* — **A failed renderer initialization shuts the renderer down.**
Was: `App::Initialize` reset the window and returned when
`VulkanRenderer::Initialize` failed, so whatever the renderer had created
before the failure was never freed. Now: it calls `mRenderer.Shutdown()` first,
then resets the window, which the renderer's surface needs until its own
shutdown.

*2026-10-02* — **Terrain draw glue (#14).** Was: app did not link `terrain`.
Now: it links it privately for `src/app/TerrainRender.{h,cpp}`, which uploads
terrain tile meshes and publishes their draws. Nothing calls it
until scenes can hold terrain (#118); `App` itself is unchanged.

*2026-10-02* — **A build without the editor.** Was: `app` linked `mir::editor`
PUBLIC unconditionally, so every binary carried the editor. Now: the
`MJOLNIR_WITH_EDITOR` option, ON by default, and the `windows-msvc-game-*`
presets that turn it off. `App` gains `kHasEditor`, `GameUIArea` and, with the
editor, `ApplyEditorActions`, which holds what `Run` did inline with
`EditorActions`.

*2026-10-02* — **The game shell, and a game build plays.** Was: `App` drew the
game UI only under `--ui-demo`, and with `--no-editor` the world stayed in Edit,
so no script ran. Now: the game UI runs every frame and draws `GameShell`, the
HUD and pause menu, in Play; `ApplyShellAction` carries out what the player
asked after the UI's frame; and `Initialize` calls `PlayScene` without the
editor. `App` still never touches `InputState::SetUiCapture` for the game UI.

*2026-09-18* — **`GameProject` reads through `mir::serial`, and owns the project
filenames.** Was: `ReadDefaultGame` and `LoadGameProject` opened their own
`std::ifstream` and parsed with `operator>>`, and `"engine.json"`, `"game.json"`
and `"project_settings.json"` were literals spread across the file. Now they call
`ReadJsonFile`, and the three names are `kEngineManifestFileName`,
`kGameManifestFileName` and `kProjectSettingsFileName` in `GameProject.h`.

`LoadGameProject` tests for `game.json` with `is_regular_file` before reading it.
`ReadJsonFile` returns `nullopt` for both a missing and a malformed file, and this
function needs them apart: a missing manifest means the directory is not a game,
while a malformed one means a game whose manifest falls back to defaults.

`App` no longer spells `"scene.json"` or `"manifest.blob"`. It asks
`SceneManifestPath` (see [scene.md](scene.md)) and uses `kAssetManifestFileName`
(see [assets.md](assets.md)) instead.

`SceneResolve`'s mesh parsing uses `ParseJson`, which takes the asset id as the
context for its error message, since a cooked blob has no path to report.

*2026-09-17* — **`RenderSettings` loaded alongside `InputMap`.** `Initialize`
now also emplaces a `RenderSettings` World resource, loaded from the same
`project_settings.json` (see [renderer.md](renderer.md#changed) — the file
gained a `"rendering"` section next to `InputMap`'s `"input"`).

*2026-09-15* — **Fixed timestep for `SystemPhase::FixedUpdate`.** Was: `Run`
ticked every phase once per frame with the variable `dt`, so physics-style
systems had no stable substep. Now: `Run` advances a `FixedTimestep`
(`AppDesc::mFixedDeltaSeconds`, 1/60 s, at most `mMaxFixedSteps`, 4, a frame)
and hands the substep count to `SystemScheduler::Update`, which runs
`FixedUpdate` that many times between `Update` and `PostUpdate` with the fixed
step as `dt`, flushing after each. Every other phase still runs once with the
frame `dt`, so `Render` happens exactly once per frame.

The loop owns the clock and the scheduler owns the order: the scheduler takes a
step count and keeps no time state. Leftover time carries to the next frame,
but the backlog is clamped to one capped frame, so a sustained overload becomes
slow motion instead of a growing debt. The accumulator advances in every mode;
a `FixedUpdate` system checks `IsSimulating` itself, as `ScriptSystem` does, so
Play and Unpause start without a catch-up burst.

*2026-09-14* — **The game is chosen at launch, and switching games restarts.**
Was: `AppDesc::mProjectPath` named `games/HelloWorld/game.json`, parsed by a
private `GameProject` in `App.cpp`, so opening another game meant editing
`main.cpp`. Now: `GameProject` is public in `app/GameProject.h`, `main.cpp`
picks the game from `--game` or `engine.json` and passes it as
`AppDesc::mProject`, and the resolve passes are free functions in
`src/app/SceneResolve.cpp`. Project > Open Project calls `RequestOpenProject`,
and `main.cpp` relaunches with `--game`.

An in-process switch was built first and dropped. Script-spawned entities, Lua
globals and script-declared component types all outlived it, and clearing each
one would have meant reaching into `core` for every process-wide singleton.

*2026-09-14* — **`AppDesc::mMaxFrames`.** Was: `Run` ended only when someone
closed the window, so nothing could drive the engine unattended. Now: after
`mMaxFrames` frames `Run` calls `Window::RequestClose`, the same exit as the
close button. `main.cpp`'s `--frames` sets it for the `smoke.<game>` tests.

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
