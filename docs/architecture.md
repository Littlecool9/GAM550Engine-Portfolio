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

`script` privately links `terrain` for the resident Lua height query (#144);
`terrain` retains its assets/core dependencies and no script dependency, and
links `serial` privately for its streaming settings (#126).

The folder tree is in the [README](../README.md#folder-structure). This is how
the pieces depend on each other.

```
               MjolnirEngine (main.cpp)
                         |
                        app
      +--------+---------+--------+-------------+
      |        |         |        |             |
   assets    scene    script    editor      gameshell
      |        |         |      |    |          |
      |        |         |      | editortheme gameui
      |        |         |      |               |
      |        |         |   imguiruntime     input
      |        |         |     |      |         |
      |        |         |  window  renderer    |
      |        |         |     |      |         |
      +--------+---------+-----+------+---------+
                         |
                       core
```

`app` also links `window`, `renderer` and `imguiruntime` directly, because it
constructs them. It links `editor` only when `MJOLNIR_WITH_EDITOR` is on; a
build with it off has no `editor`, `editortheme` or ImGuizmo at all (see
[app.md](modules/app.md#without-the-editor)). `editor` links `window` and `renderer` too, for the title bar
and the viewport rect. `gameui` sits beside `editor` and draws into the same
ImGui frame, but links `imgui` alone, privately, and neither `editor` nor
`imguiruntime`: it never owns or brackets the frame. It also links `assets`,
for the cooked textures it draws, and not `renderer`: ImGui's backend uploads
those itself. `gameshell`, the HUD and pause menu every game gets, links
`gameui` alone, as a game's own screens would.

`serial` is not shown: `renderer`, `scene`, `input` and `app` reach `core`
through it for anything touching a JSON file.

| Module | Owns | Document |
|---|---|---|
| `core` | ECS, logging, paths, the surface contract | [core.md](modules/core.md) |
| `window` | An OS window and its event pump | [window.md](modules/window.md) |
| `renderer` | The whole Vulkan path, and render-facing components | [renderer.md](modules/renderer.md) |
| `assets` | Cooked blobs, manifest, cache | [assets.md](modules/assets.md) |
| `animation` | Model pose sampling and per-instance playback data | [animation.md](modules/animation.md) |
| `scene` | Scene save and load | [scene.md](modules/scene.md) |
| `serial` | JSON documents on disk | [serial.md](modules/serial.md) |
| `script` | Lua embedding and ECS bindings | [script.md](modules/script.md) |
| `audio` | Wwise lifetime and the per-frame pump | [audio.md](modules/audio.md) |
| `imguiruntime` | ImGui context lifetime, fonts, both backends, the frame bracket | [imguiruntime.md](modules/imguiruntime.md) |
| `editor` | The editor shell, drawn into `imguiruntime`'s context | [editor.md](modules/editor.md) |
| `editortheme` | Theme data only | [editortheme.md](modules/editortheme.md) |
| `gameui` | Game UI drawn over the scene, the same in the editor and a game build | [gameui.md](modules/gameui.md) |
| `gameshell` | The HUD and the pause menu every game gets, built on `gameui` | [gameshell.md](modules/gameshell.md) |
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

**`imguiruntime` owns the ImGui context's lifetime, and nothing else does.**

It creates the context, loads the fonts, initialises the GLFW backend and,
through `VulkanRenderer::InitImGuiVulkanBackend`, the Vulkan one, then tears all
of it down in reverse. It also brackets the frame: `BeginFrame` is the
`NewFrame` trio and `EndFrame` is `Render` plus `GetDrawData`. Every UI module,
`editor` today and `gameui` next, draws between those two calls and owns no part
of the lifetime.

It is its own module because neither obvious owner may hold it.
`ImGui_ImplGlfw_InitForVulkan` needs a `GLFWwindow *`, which `renderer` may not
see, and `App.cpp` may not call ImGui.

`editor` keeps what only an editor wants: the theme and its DPI scale, docking,
and `imgui.ini`. The runtime leaves `IniFilename` null, so a build without
`editor` persists nothing.

`app` drives a frame as `ImGuiRuntime::BeginFrame`, `Editor::DrawLayout`,
`UIContext::BeginFrame`/`EndFrame`, `ImGuiRuntime::EndFrame`, then reads
`Editor::SceneViewportRect`. `App.cpp` contains no ImGui call and includes no
ImGui header.

**`gameui` draws into ImGui's background draw list and nowhere else.**

That list renders before every window, and the renderer draws ImGui after the
3D views, so the game UI lands over the scene and under the editor's panels. The
same code is a HUD clipped to the scene viewport in the editor and fullscreen UI
in a game build, with no renderer change. Only `UIBackendImGui.cpp` includes
ImGui, and no public `gameui` header names an ImGui type.

**`editortheme` depends on nothing engine-side, only `imgui`.**

It is theme data, applied once by `editor` at startup.

**File bytes move in `core`, and JSON in `serial`.**

`core/fs/FileIO.h` opens every file the engine reads or writes, and
`std::ifstream` and `std::ofstream` appear in no other translation unit.

`serial/JsonFile.h` is the only place that decides those bytes are JSON. It is
its own module rather than part of `core` because it returns `nlohmann::json`,
which would otherwise make that library a public dependency of the module
everything else depends on.

A module above those two decides what a document means, and names no stream
type. The flow is in [A file](#a-file).

## Flows

### A frame

`App::Run` is a variable-dt loop; only the `FixedUpdate` phase steps at a fixed rate.

```
PollEvents                       close flag and framebuffer size refresh here
dt = min(elapsed, mMaxDeltaSeconds)
TerrainRuntime::Update           installs finished terrain reads, mesh builds, uploads and collision pieces (every mode); then, in Edit, its Refresh
ImGuiRuntime::BeginFrame         the NewFrame trio
Editor::DrawLayout               -> EditorActions (scene action, game to open)
UIContext::BeginFrame/EndFrame   game UI into the background list, scene viewport or whole window
ImGuiRuntime::EndFrame           -> ImDrawData*
renderer.SetImGuiDrawData(...), renderer.SetSceneViewport(...)
ScriptReloadWatcher::Poll        mtime check, throttled
FixedTimestep::Advance(dt)       substeps this frame, mFixedDeltaSeconds each, at most mMaxFixedSteps
SystemScheduler::Update          PreUpdate, Update, FixedUpdate x substeps, PostUpdate, Render
```

Ordering between features is expressed as a phase, never as registration order.

`ScriptSystem` runs in `PreUpdate`. `FixedUpdate` is for physics and its sync
back to `Transform`; it runs zero or more times a frame with the fixed step as
`dt`. Before each step `PhysicsSystem` calls the `PhysicsStepGates` World
resource's gates; app's terrain is one, placing collision pieces and holding
bodies whose ground is not collidable yet (#128). `TransformPropagateSystem` runs in `PostUpdate`, after it. `RenderSystem`
runs in `Render`, after all of them, so every `WorldTransform` it reads is
already current for the frame being drawn.

The scheduler flushes the command buffer at each phase boundary and after each
`FixedUpdate` substep, never in the middle of a system.

`RenderSystem` holds a `VulkanRenderer&` passed to its constructor. The renderer
is not published as a world resource, which keeps it out of reach of scripts.
Draws that depend on the viewport come from the `ViewDrawSources` resource:
`RenderSystem` hands each source every viewport's camera before it builds any
view, and app's terrain answers per view with its own level of detail (#125).
Meshes queued with `QueueMesh` are copied at the start of the next recorded
frame within a per-frame byte budget and become drawable once that frame has
completed; retired ones are freed after their last frame, with no wait (#127).

### A file

Naming a file and moving its bytes are separate mechanisms. A path is resolved
once at startup into a `GameProject`, and bytes always travel the same three
layers.

A game directory is named by `--game` or by `defaultGame` in `engine.json`, and
every path is built from `ExecutableDir()`.

```
--game <dir>      engine.json "defaultGame"
         \             /
     NormalizeGameDir()          rejects anything outside ExecutableDir()
             |
     LoadGameProject()           reads game.json: assetsRoot, sceneDir, soundBanksRoot
             |
        GameProject
             |
             +-- mProjectSettingsPath --> InputMap, RenderSettings
             +-- mSceneDir            --> SceneManifestPath(), EntityFilePath()
             +-- mSoundBankDir        --> audio::Init, after which Wwise opens banks itself
             +-- mAssetIdRoot         --> an id prefix, not a disk path
```

`GameProject` holds the directories while each module holds its own filenames, so
`app` joins a root to a constant at the call site. `kSceneManifestFileName` lives
in `scene`, `kAssetManifestFileName` in `assets`, and the three project-level
names in `app/GameProject.h`. A module that spells another module's filename
prevents that module from renaming its own file.

A key missing from `game.json` falls back to a default in `GameProject.cpp`. The
`assetsRoot` default is written a second time in the root CMakeLists, which reads
the same key at configure time to decide what to cook, and the two have to agree
or the cooker and the runtime hash ids from different trees.

Cooked assets are named the other way round, by id rather than by path.

```
scene field  "meshes/cube.mesh.json"
             |
     mAssetIdRoot + "/" + name
             |
     MakeAssetId()               FNV-1a 64 of the path string
             |
     AssetManifest::Find(id)
             |
     CookedAssetsDir() / PathOf(entry)
```

A scene names an asset by a path that is never opened. The string is hashed into
an `AssetId`, the manifest turns that id into a cooked file, and a real path
exists only at that last step ([An asset](#an-asset)). That hash is also why
`assetsRoot` cannot change once a build has cooked.

Bytes then move through one stack, whichever lane named the file.

```
GameProject  InputMap  RenderSettings  SceneIO    SceneResolve   AssetCache   VulkanRenderer
     |          |            |            |            |              |             |
 ReadJsonFile / WriteJsonFile / UpdateJsonFile      ParseJson         |             |
     |          |            |            |         (no path)        |             |
     +----------+------------+------------+                          |             |
                |                                                    |             |
      ReadText / WriteTextAtomic                                 ReadBytes     ReadBytes
                |                                                    |             |
                +----------------------------------------------------+-------------+
                                        |
                             std::ifstream / std::ofstream
```

`ReadJsonFile` is `ReadText` followed by `ParseJson`, and `WriteJsonFile` is
`dump()` followed by `WriteTextAtomic`, so the `try`/`catch` around a parse
appears once in the engine. `AssetCache` and `ReadSpirv` skip `serial` because
they want bytes rather than a document. `SceneResolve` holds bytes already, out
of an `AssetBlobView`, so it calls `ParseJson` with the asset id as the context
an error names.

Writes through `serial` are atomic, because JSON on disk here is always a file a
person can edit. `AssetCooker` writes with plain `WriteBytes`, since a tool
regenerates its output.

A missing file is quiet and a malformed one is logged, so a caller that needs
them apart tests for the file first. `LoadGameProject` does, because a missing
`game.json` means the directory is not a game while a malformed one means a game
that falls back to defaults.

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

Scenes are not a cooked asset - `SceneIO` reads/writes them directly under
`games/<name>/scenes/<sceneName>/`, next to the executable (`GameProject`
resolves every path there against `ExecutableDir()`, not the repo), because
`SaveScene` has to write them back at runtime. The root CMakeLists seeds
that directory (and `project_settings.json`) from the repo's checked-in
copy once, at first configure, and never touches it again: unlike
`assets/`/`game.json`, which have no runtime writer and safely re-mirror
from source on every build, doing that to `scenes/` would silently discard
whatever the running editor last saved the moment the project is next
rebuilt.

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
4. Linux sources exist, but there is no Linux preset. macOS fails at configure.
5. No CI. Tests are run by hand.

## Changed

*2026-10-02* — **The editor is optional.** Was: `app` linked `editor`
unconditionally. Now: `MJOLNIR_WITH_EDITOR` OFF, set by the
`windows-msvc-game-*` presets, builds `MjolnirEngine` as the game with no
editor, `editortheme` or ImGuizmo in the build, which is what moving ImGui's
lifetime into `imguiruntime` made possible. See [build.md](build.md#a-build-without-the-editor).

*2026-10-02* — **`gameshell` added.** Was: the only game UI was
`gameui`'s own demo screen. Now: a HUD and a pause menu, in a module that
links `gameui` alone, drawn by `App` in Play, and a game build started with
`--no-editor` begins in Play. See [gameshell.md](modules/gameshell.md).

*2026-10-02* — **`gameui` links `assets` instead of `renderer`.** Was: it
linked `renderer` for `TextureHandle`, which had no way into ImGui. Now: it
loads cooked textures from the `AssetCache` and hands them to ImGui 1.92's
dynamic texture path, which the Vulkan backend uploads and frees. See
[gameui.md](modules/gameui.md).

*2026-09-27* — **`gameui` added.** Was: `editor` was the only thing drawing into
the ImGui frame, so a game had no UI. Now: `gameui` records a draw list and
replays it into ImGui's background draw list, over the scene and under the
editor's panels, clipped to the scene viewport in the editor and fullscreen
without it. See [gameui.md](modules/gameui.md).

*2026-09-26* — **ImGui's lifetime moved out of `editor` into `imguiruntime`.**
Was: `Editor::Initialize` created the context, loaded the fonts and initialised
both backends, and `Editor::BeginFrame`/`EndFrame` bracketed the frame, so any
other UI module would draw into a context the editor owned and there could be no
editor-free build. Now: `imguiruntime` owns all of that, and `editor` draws
between its `BeginFrame` and `EndFrame` as `gameui` will. See
[imguiruntime.md](modules/imguiruntime.md).

*2026-09-19* — **Descriptor sets and a per-frame camera UBO replaced the
premultiplied view-projection push constant.** Was: `RenderSystem` baked
`viewProj * world.Matrix()` into `DrawItem::model` on the CPU, so the camera
travelled as part of the 128-byte push constant. Now: `DrawItem::model` is
just the world transform, `RenderView` carries its own `viewProj`, and
`VulkanRenderer` writes it into a per-frame, per-view slice of a dynamic
uniform buffer (set 1) that the vertex shader combines with the model push
constant. See [renderer.md](modules/renderer.md).

*2026-09-18* — **File bytes moved to `core`, and JSON to `serial`.** Was:
`assets/AssetFileIo.h` held the byte primitives, so `input` and `renderer` linked
`mir::assets` to read a config file that is not an asset, and `app`, `scene` and
the SPIR-V loader opened their own streams besides, each with its own
`json::parse` try/catch and its own log wording. Now: `core/fs/FileIO.h` and
`serial/JsonFile.h`, with the flow in [A file](#a-file) and the per-module detail
in [core.md](modules/core.md#changed) and [serial.md](modules/serial.md).

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
