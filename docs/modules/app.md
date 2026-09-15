# app

The composition root and the frame loop. `App` is the one place that knows every
other module exists. It contains no gameplay and no subsystem of its own, and it
is not a framework — there is no registry, no service locator, and no virtual
`Game` to subclass.

`mir::app` · depends `mir::core`, `assets`, `window`, `renderer`, `editor`, `scene`, `script`, all public; `input`, `nlohmann_json` private · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-09-06

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
    Editor::BeginFrame
    Editor::DrawLayout -> EditorActions      New / Save / Load and Open Project acted on here
    renderer.SetImGuiDrawData(Editor::EndFrame())
    renderer.SetSceneViewport(Editor::SceneViewportRect())
    ScriptReloadWatcher::Poll
    SystemScheduler::Update            PreUpdate, Update, PostUpdate, Render
    ++frame
    halfway through mMaxFrames, if mSwitchProjectTo: RequestOpenProject
    if mMaxFrames reached: RequestClose
```

`App` contains no ImGui call and never builds a `DrawItem`. The editor produces a
draw-data pointer and a rect, and `RenderSystem` does the drawing from the
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

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size, Vulkan app name and validation, editor-layout gate, `dt` clamp, frame limit, the game to open, a scripted game switch for tests |
| `App` | `app/App.h` | Owns the subsystems, runs the loop, takes a request to open another game |
| `GameProject` | `app/GameProject.h` | `{mDir, mName, mTitle, mAssetIdRoot, mSceneDir, mProjectSettingsPath}` for one game directory |

`app/GameProject.h` also declares `ReadDefaultGame`, `NormalizeGameDir`,
`LoadGameProject`, `ResolveGameProject` (normalize, then load) and
`DiscoverGameProjects` (every `games/*/game.json` next to the executable).

Accessors: `GetWorld()`, `Systems()`, `Scripts()`, `ScriptReload()`, `Splash()`,
`Assets()`, `Scene()`, `Project()`, `AvailableProjects()` and `ProjectToOpen()`.
`Renderer()` is marked a temporary seam, because `Renderer.h` is still an empty
abstraction placeholder.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds. `NewScene` and `LoadScene` both
release the discarded scene's GPU meshes and materials. Game control is
`RequestOpenProject(dir)`, and the switch itself is a restart.

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

## State

*As of 2026-09-06.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, Project > Open Project restarts
into another game, and the scene renders.

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
2. **No fixed timestep**, so physics-style systems have no stable substep.
   Undecided whether that belongs in the scheduler or in the loop.
3. **No input, no pause, no frame limiter**, and no window-event handling beyond
   the renderer re-querying size.
4. **`Initialize` after `Shutdown`** is written to work — the sticky asset flag
   and the explicit resets exist for it — but nothing exercises it.
5. One `App` per process is assumed everywhere, and nothing enforces it.

## Changed

*2026-09-14* — **`App` owns its game, and switching games restarts.** Was:
`main.cpp` resolved one game at startup and ran the scene resolve passes itself,
so a game could only be chosen at launch, and File > Load Scene left the loaded
scene's meshes and scripts unresolved. Now: `GameProject` and the resolve passes
live in this module and `LoadScene` resolves every time. Project > Open Project
calls `RequestOpenProject`, and `main.cpp` relaunches with `--game`.

An in-process switch was built first and dropped. Script-spawned entities, Lua
globals and script-declared component types all outlived it, and clearing each
one would have meant reaching into `core` for every process-wide singleton.

*2026-09-14* — **`AppDesc::mMaxFrames`.** Was: `Run` ended only when someone
closed the window, so nothing could drive the engine unattended. Now: after
`mMaxFrames` frames `Run` calls `Window::RequestClose`, the same exit as the
close button. `main.cpp`'s `--frames` sets it for the `smoke.<game>` tests.

*2026-09-03* — **Drawing left `App::Run`.** Was: `CollectDrawInstances` and
`DrawFrame` called by hand between scheduler phases, so draw-list assembly had no
defined ordering against the systems that produce what it draws. Now:
`RenderSystem` in `SystemPhase::Render`, taking the renderer by constructor
reference. `App::Run` became poll, edit UI, poll reload, update.

*2026-09-03* — **ImGui left `App.cpp`.** Was: about 150 lines of `ImGui::*` and
`ImGui_Impl*` calls next to window and ECS orchestration, with `App.h` needing
dockspace internals just to declare a member. Now: an `Editor` member and four
calls, and `App.cpp` includes no ImGui header.
