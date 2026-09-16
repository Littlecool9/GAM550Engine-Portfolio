# Architecture

*State as of 2026-09-06.* An archetype ECS, a Vulkan 1.3 renderer, an offline
asset pipeline, a Lua scripting layer, an ImGui editor shell, and a thin app
that composes them.

## Principles

**One static library per module.**

Every part of the engine compiles to its own static library under `modules/` for modularity.\
Each module declares its dependencies in its own `CMakeLists.txt`, and the
linker enforces them. A cycle of dependencies will fail the build.

**Expose the bare minimum.**

*Modules*:
Each module has `include/<module>/` for what other consumers may use, and
`src/<module>/` for everything else.

*Third-party libraries*: Libraries should be linked `PRIVATE` by default.If technical exception arises, document and share with team.

**A game is data, not a build target.**

A game lives in `games/<name>/` as configuration, assets, Lua scripts and scene
files. It contains no C++ and adds no target to the build.

**Components are POD, and anything that owns memory is referenced by handle.**

A component must be trivially copyable and standard layout. 

This is because entities are grouped into archetype tables by their exact component set, so adding or removing a component moves that entity's row into a different table.
With POD components, that move is a `memcpy` per column. 

Data that owns memory lives in a world resource, or in the module that manages it, and the component carries a handle to it. More in [core.md](modules/core.md)

**Assets are cooked offline.**

`tools/AssetCooker` converts source files into one container format at build
time. The runtime reads only that format.

An `AssetId` is a hash of the source path relative to the repository root, so the
same file gets the same id on every machine, and any code can compute an id from
a path.

**Failures return a value.**

Engine code does not use exceptions.

An operation that can fail returns `false`, `nullptr` or `std::optional`, and
logs the reason.

Programmer error uses assertions instead. `MIR_ASSERT` is for development errors and stripped in Release.
`MIR_CHECK` is not stripped an used for runtime-errors for bad inputs. [conventions.md](conventions.md) has the full rule.

## Module graph

The folder tree is in the [README](../README.md#folder-structure). This is how
the pieces depend on each other.

```
               MjolnirEngine (main.cpp)
                         |
                        app
      +--------+---------+--------+--------+--------+
      |        |         |        |        |        |
   assets   window   renderer  editor    scene   script
      |        |         |        |        |        |
      +--------+---------+--------+--------+--------+
                         |                 |
                       core           editortheme
```

| Module | Owns | Document |
|---|---|---|
| `core` | ECS, logging, paths, the surface contract | [core.md](modules/core.md) |
| `window` | An OS window and its event pump | [window.md](modules/window.md) |
| `renderer` | The whole Vulkan path, and render-facing components | [renderer.md](modules/renderer.md) |
| `assets` | Cooked blobs, manifest, cache | [assets.md](modules/assets.md) |
| `scene` | Scene save and load | [scene.md](modules/scene.md) |
| `script` | Lua embedding and ECS bindings | [script.md](modules/script.md) |
| `audio` | Wwise lifetime and the per-frame pump | [audio.md](modules/audio.md) |
| `editor` | ImGui context, backends and the editor shell | [editor.md](modules/editor.md) |
| `editortheme` | Theme data only | [editortheme.md](modules/editortheme.md) |
| `app` | Composition root and the frame loop | [app.md](modules/app.md) |

Add your row when you start a document, and update it in the same commit as the
code it describes.

## Cross-module rules

**`core` depends on nothing in the engine.**

Everything else may depend on it.

**`app` is the only module that knows all the others.**

It holds each subsystem as a named member. Declaration order in `App.h` is
initialisation order, and reverse declaration order is destruction order, so the
order in that header is what makes teardown correct.

Adding a subsystem means editing `App`, which forces a decision about where it
belongs in that order.

**`renderer` never sees `window`.**

It takes an `ISurfaceProvider`, declared in `core/platform/Surface.h`, which
exposes `NativeWindow()`, `Width()` and `Height()`.

`engine_renderer` does not link `engine_window`, and no GLFW type appears in a
renderer translation unit.

Anything that can produce a native handle can therefore drive the renderer.
Anything else the renderer comes to need — DPI scale, HDR — is added to
`ISurfaceProvider` deliberately.

**Render-facing components live in `renderer`.**

`MeshRenderer` and `Camera` hold handles that the renderer allocates, validates
and destroys, so they are declared in `renderer/include/renderer/components/`.

`engine_core` never depends on `engine_renderer`. A consumer of `core` that does
not render never pulls Vulkan in through a component header.

A module adding its own render-facing component follows the same rule.

**`editor` owns ImGui end to end.**

That covers the context, fonts, theme, both backends and the dockspace.

`app` drives it through `BeginFrame`, `DrawLayout`, `EndFrame` and
`SceneViewportRect`. `App.cpp` contains no ImGui call and includes no ImGui
header.

**`editortheme` depends on nothing engine-side, only `imgui`.**

It is theme data, applied once by `editor` at startup.

## Flows

### A frame

`App::Run` is a variable-dt loop; only the `FixedUpdate` phase steps at a fixed rate.

```
PollEvents                       close flag and framebuffer size refresh here
dt = min(elapsed, mMaxDeltaSeconds)
Editor::BeginFrame / DrawLayout / EndFrame -> ImDrawData*, EditorActions (scene action, game to open)
renderer.SetImGuiDrawData(...), renderer.SetSceneViewport(...)
ScriptReloadWatcher::Poll        mtime check, throttled
FixedTimestep::Advance(dt)       substeps this frame, mFixedDeltaSeconds each, at most mMaxFixedSteps
SystemScheduler::Update          PreUpdate, Update, FixedUpdate x substeps, PostUpdate, Render
```

Ordering between features is expressed as a phase, never as registration order.

`ScriptSystem` runs in `PreUpdate`. `FixedUpdate` is for physics and its sync
back to `Transform`; it runs zero or more times a frame with the fixed step as
`dt`. `TransformPropagateSystem` runs in `PostUpdate`, after it. `RenderSystem`
runs in `Render`, after all of them, so every `WorldTransform` it reads is
already current for the frame being drawn.

The scheduler flushes the command buffer at each phase boundary and after each
`FixedUpdate` substep, never in the middle of a system.

`RenderSystem` holds a `VulkanRenderer&` passed to its constructor. The renderer
is not published as a world resource, which keeps it out of reach of scripts.

### An asset

A source file under a cooked root is read by `tools/AssetCooker` at build time.
The cooker writes one blob per file, named by its `AssetId`, plus a manifest. The
build copies both next to the executable, and `AssetCache::Load(id)` reads them
at runtime.

The cooked roots are the engine's `assets/` plus each game's `assetsRoot`. The
root CMakeLists reads those from every `games/*/game.json` at configure time, so
adding a game adds its assets without touching CMake.

An `AssetId` is the FNV-1a 64 hash of the source path relative to the repository
root. That is why `engine_cook_assets` rejects an absolute cook root: the root
string is part of what gets hashed, so an absolute path would bake one machine's
checkout location into ids that have to match on every machine.

Shaders do not go through this. They are compiled by `slangc` into `.spv` files
and copied next to the executable separately.

### A scene

`scene/SceneIO.h` writes a scene as a directory. `scene.json` lists `StableId`s
in load order, and `entities/<id>-<name>.json` holds one entity each.

Splitting by entity keeps a diff proportional to what changed, and lets two
people edit different entities in the same scene without conflicting.

Components round-trip generically, through the `FieldDesc` tables registered in
`ComponentRegistry`. Giving a component a field table is therefore the entire
serialization cost of that component, and it works the same way for a
script-declared component as for a C++ one.

The three load passes are in [scene.md](modules/scene.md).

### A script

A `.lua` file is a cooked asset. `ScriptHost` loads its source, `CreateInstance`
returns an `instanceRef`, and a `ScriptRef` component links an entity to that
instance.

`ScriptSystem` walks `Query<ScriptRef>` in `PreUpdate` and calls into Lua per
entity.

Scripts reach components by name, through `ComponentRegistry`. Exposing a new
native component to Lua therefore costs no new binding code — it costs a field
table. See [script.md](modules/script.md).

## Known gaps

1. No input. `window` surfaces no events at all.
2. No renderer abstraction. `Renderer.h` is a placeholder and `App` names
   `VulkanRenderer` directly, so there is no headless mode, and nothing in
   `app`, `renderer` or `editor` is unit tested.
3. No typed assets. Everything cooks as a raw passthrough blob.
4. No descriptor sets or uniform buffers. The camera's view-projection travels as
   a premultiplied push constant.
5. Linux sources exist, but there is no Linux preset. macOS fails at configure.
6. No CI. Tests are run by hand.

## Changed

*2026-09-14* — **The runtime loads a game directory.** Was: a `HelloWorld`
executable whose `main.cpp` hard-coded `games/HelloWorld` paths and built a demo
scene when none was saved. Now: `MjolnirEngine --game <dir>`, defaulting to
`defaultGame` in `engine.json`. Paths in `game.json` are relative to the game, a
directory without `game.json` is refused, and the root CMakeLists cooks every
game's assets and registers a `smoke.<game>` test for it.

*2026-09-03* — **`RenderSystem` replaced hand-driven drawing.** Was: `App::Run`
called `CollectDrawInstances` and `DrawFrame` by hand between scheduler phases,
so draw-list assembly had no defined ordering against the systems that produce
what it draws. Now: a normal system in `SystemPhase::Render`. Ordering comes from
phase placement, and `App::Run` no longer knows a `DrawItem` exists.

*2026-09-03* — **ImGui moved out of `App` into `editor`.** Was: about 150 lines
of `ImGui::*` and `ImGui_Impl*` calls in `App.cpp`, with `App.h` needing
dockspace internals just to declare a member. Now: a module with a six-call
surface.

*2026-09-03* — **The scene scissor rect comes from the dockspace central node.**
Was: read from `ImGui::Begin("Scene")`'s content region. That compiled and ran
with no warning, and clipped the scene to a 300x650 sidebar, so the cube rendered
into nothing. Now: read only from `DockBuilderGetCentralNode` and exposed as
`Editor::SceneViewportRect()`, and no window named `"Scene"` exists. Anyone
adding a real named viewport window has to update that source, or the same bug
comes back silently.
