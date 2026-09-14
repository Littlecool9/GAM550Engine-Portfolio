# app

The composition root and the frame loop. `App` is the one place that knows every
other module exists. It contains no gameplay and no subsystem of its own, and it
is not a framework — there is no registry, no service locator, and no virtual
`Game` to subclass.

`mir::app` · depends `mir::core`, `assets`, `window`, `renderer`, `editor`, `scene`, `script`, all public · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-09-06

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
    Editor::DrawLayout -> SceneMenuAction    New / Save / Load acted on here
    renderer.SetImGuiDrawData(Editor::EndFrame())
    renderer.SetSceneViewport(Editor::SceneViewportRect())
    ScriptReloadWatcher::Poll
    SystemScheduler::Update            PreUpdate, Update, PostUpdate, Render
    ++frame
    if mMaxFrames reached: RequestClose
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
desc.mTitle = "MjolnirEngine";

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

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size and title, Vulkan app name and validation, editor-layout gate, `dt` clamp, frame limit, scene directory |
| `App` | `app/App.h` | Owns the subsystems, runs the loop |

Accessors: `GetWorld()`, `Systems()`, `Scripts()`, `ScriptReload()`, `Splash()`,
`Assets()` and `Scene()`. `Renderer()` is marked a temporary seam, because
`Renderer.h` is still an empty abstraction placeholder.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds.

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

## State

*As of 2026-09-06.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, and the scene renders.

The renderer is a value member rather than a `unique_ptr`, because there is no
interface to hold.

No unit tests. `App` is composition, and its behaviour is a window opening, a
device initialising and a loop running. The parts worth asserting — phase
ordering, command buffer flushing, asset lookup, scene round-tripping — are
tested where they live. A headless renderer is the prerequisite for unit testing
anything here. Until then the `smoke.<game>` tests run the whole `App` for 120
frames per game, through `mMaxFrames`, and fail on a crash or a logged error.

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
