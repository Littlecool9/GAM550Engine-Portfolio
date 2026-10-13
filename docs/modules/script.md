# script

Embeds Lua as the gameplay scripting layer, through sol2 over LuaJIT. A script
reads per-frame ECS state and drives triggers, entity behaviour and UI logic. It
does not touch physics, flight dynamics or rendering.

`mir::script` · depends `mir::core` (public); `mir::assets`, `mir::audio`, `mir::input`, `mir::physics`, `mir::terrain`, `sol2`, `luajit` (private) · API `modules/script/include/script/` · maintainer Rahul Nair · reviewed 2026-09-18

## Model

Exposing a new native component to Lua costs no new binding code. This module is
the main consumer of `core`'s `ComponentRegistry` and `FieldDesc` name erasure,
which was built for exactly this. Scripts reach components by name, so the
bindings never mention a C++ component type.

There are two lifetimes, kept separate.

**Code** is one Lua table per loaded script *name*, holding only the functions
that script defines — `OnStart`, `OnUpdate`, `OnStop` and `NewInstanceData`. It
is shared across every entity running that script.

**State** is one small private table per `instanceRef`, passed as `self` to every
callback.

`ScriptRef` is a POD component carrying `instanceRef`, `started` and
`scriptName`, and it is the only thing the ECS knows about a script.

`ScriptSystem` is the sole driver. It is one `ISystem` in
`SystemPhase::PreUpdate` walking `Query<ScriptRef>`, calling `OnStart` once and
then `OnUpdate` every frame. Each entity's callback is isolated from the others
by a C++ `try`/`catch` and by sol2's `protected_function`, so a script error
stops that entity's script rather than the frame.

```cpp
const AssetId id = MakeAssetId("games/HelloWorld/assets/scripts/spin.lua");
const AssetBlobView *blob = app.Assets()->Load(id);

app.Scripts().LoadScriptSource("spin", AsStringView(*blob));
app.ScriptReload().Track("spin", id);          // picks up an edit on the next cook

const int32_t instance = app.Scripts().CreateInstance("spin");
world.AddComponent<ScriptRef>(entity, ScriptRef{.instanceRef = instance});
```

```lua
local Spin = {}
local speed = 1.0

function Spin.NewInstanceData()
    return { angle = 0.0 }              -- per-instance; never shared
end

function Spin.OnUpdate(self, world, entity, dt)
    self.angle = self.angle + speed * dt
    local half = self.angle * 0.5
    world:set(entity, "Transform", "rotation", { w = math.cos(half), x = 0.0, y = math.sin(half), z = 0.0 })
end

return Spin
```

## API

| Type | Header | Role |
|---|---|---|
| `IScriptHost` | `script/IScriptHost.h` | The abstract seam — the only thing outside `script/src` should depend on |
| `ScriptHost` | `script/ScriptHost.h` | The one Lua-backed implementation; sol2 hidden behind a pimpl |
| `ScriptRef` | `script/components/ScriptRef.h` | POD link component: `instanceRef`, `started`, `scriptName` |
| `ScriptSystem` | `script/ScriptSystem.h` | Drives the callbacks per `ScriptRef`, `SystemPhase::PreUpdate` |
| `ScriptReloadWatcher` | `script/ScriptReloadWatcher.h` | Polls a tracked script's cooked file and reloads on change |
| `RegisterScriptComponents` | `script/ComponentRegistration.h` | Registers `ScriptRef`, mirroring `core` and `renderer` |

The Lua surface — `world:has/get/set/spawn/destroy/add/remove/each/declare` and
the `Entity` usertype — lives in `src/script/bindings/` and is not public. Loaded
Lua source reaches it; no C++ outside this module includes it. The input,
physics, audio and terrain bindings each extend that same `World` usertype rather than
adding one of their own, and each is registered after `RegisterWorldBindings`.

The input half is `world:is_down/just_pressed/just_released/axis/axis2d` and
`world:active_device`. Scripts are gameplay, so the action reads go through
`InputState::ForGame`: every action reads at rest while a game UI screen, such
as the pause menu, holds the input. See [gameui.md](gameui.md).

The audio half is `world:post_event/stop_event/set_rtpc/set_switch/set_state`.
All but `set_state` take the entity and reach the `AkGameObject` its
`AudioEmitter` carries; `set_state` takes none, because a Wwise state is global.

```lua
function T.OnUpdate(self, world, entity, dt)
    self.charge = math.min(self.charge + dt, 1.0)
    world:set_rtpc(entity, "Speed", self.charge * 100.0)   -- false if refused
end
```

### Terrain height query (#144)

`world:terrain_height_at({x=..., y=..., z=...}, requiredSpacing)` queries the
resident `TerrainWorld` at a world position. `requiredSpacing` is the largest
acceptable spacing in meters. All three coordinates and spacing must be Lua
numbers; coordinates must be finite and fit `WorldReal`, and spacing must be
finite and positive. Missing fields, wrong types, missing or extra arguments
log an error and return `Failed` / `InvalidInput`, even without terrain.
Double builds preserve Lua doubles through `WorldPosition::FromDoubleMeters`.

```lua
local ground = world:terrain_height_at({x=150000.500123, y=10, z=-150000.250321}, 1)
if ground.hasHeight then
    print(ground.worldHeight, ground.spacing, ground.maxError, ground.releaseId)
end
```

| Result field | Meaning |
|---|---|
| `status` | `MeetsRequirement`, `CoarserWhileLoading`, `CoarserAtSourceLimit`, `Loading`, `OutsideMap`, or `Failed` |
| `failure` | `None`, `WrongRelease`, `InvalidInput`, `LoadFailed`, or `InvalidSamples` |
| `hasHeight` | True for the three height-bearing statuses |
| `localHeight` | Final terrain-local height; nil without a height |
| `worldPoint`, `worldHeight` | `{x,y,z}` surface point and its world y; nil without a point |
| `spacing`, `sourceSpacing` | Answering and finest baked spacing; nil when C++ has no finite value |
| `maxError` | C++ bound against the finest baked surface; nil when unmeasured |
| `releaseId` | Lowercase 16-digit hexadecimal release id |
| `release` | Full identity: hexadecimal `assetId`, `sourceFingerprint`, `releaseId`, numeric `formatVersion` |
| `tile`, `layer` | `{x,z}` signed tile and zero-based layer, exactly as returned by C++ |

Unknown identities retain the C++ zero sentinel as `0000000000000000`.
Missing heights never become zero; a real zero remains a number. Tile and
layer fields retain C++ defaults on failures and are meaningful with the
corresponding status. There is no sample/cell field in `TerrainHeightResult`.

Each call looks up the World resource again; it caches no release and performs
no IO. A loading state enqueues the C++ query's deduplicated payload request,
which the terrain `TerrainStreamer` reads on a worker after its next per-frame
`Update`; this binding starts no loader itself. An Eager release's payloads are
requested when the scene loads but arrive on their own, so the first frames may
answer `Loading` there too. A Play session keeps its release when `current` changes; app's Edit
safe point replaces the resource after Stop. No resource, an empty resource,
or no instance covering the point gives `OutsideMap` / `None`.

## Rules

**A script-posted voice is handed to `AudioEmitterSystem`, not kept here.**
`world:post_event` writes the playing ID it got back into the emitter's
`mPlayingId`, because that system is the only thing that stops a voice when play
mode ends or the entity is destroyed, and it holds exactly one ID. A voice left
out of it keeps playing past the session with nothing able to reach it. The
consequence is that one emitter carries one voice: a post stops whatever that
emitter was already playing, including its own `eventName`.

**Every audio binding resolves the `AkGameObject` again on each call.** An
`AudioEmitter` has none until `AudioEmitterSystem`'s first update registers one,
so a handle cached at script start would stay 0 for the entity's whole life. An
entity with no emitter, or one not registered yet, gets a refusal — `false`, or
`0` from `post_event` — rather than an error, the same contract the physics
bindings have for an entity with no `RigidBody`.

**Script state never lives in a component.** Components stay POD, so a script's
data lives in the host's instance map, addressed by `instanceRef`.

**Instance state comes from `NewInstanceData()`, never from copying a template
table.** Lua's `{}` literal already gives every instance an independent copy of
any nested table, and a C++-side shallow copy could not do that without
reimplementing a deep copy.

**Callbacks are re-resolved from the code map on every call**, rather than cached
at instance creation. That is what makes a hot-reload reach an already-running
entity on its very next tick, instead of only entities created after the reload.

**Every Lua callback goes through `sol::protected_function`**, never a raw call.
Otherwise a script's runtime error unwinds through C++ frames via Lua's own error
path instead of surfacing as a checked, loggable result. This was verified under
LuaJIT specifically, not assumed to carry over from plain Lua.

**Every structural change a script makes is mid-walk, by construction.**
`ScriptSystem` always calls into Lua from inside its own `ForEach`, so there is
no "outside a walk" case for a script to be in. The bindings route through
`core`'s `DeferredAccess.h` unconditionally rather than trying to detect which
case they are in.

**`ScriptSystem` installs a `World::AddDestroyHook` and removes it in `OnStop`.**
A hook left behind calls into freed memory on the next entity destroyed. That
pairing is covered by `modules/core/tests/System.tests.cpp`, not by a test here.

**Register native components before loading any script**, including
`RegisterScriptComponents`. `ComponentRegistry` refuses a script that claims an
already-native name, so a script loaded first would win the name instead.

**`Entity` crosses the boundary as a `sol::usertype<Entity>`, never a packed
integer.** A Lua number is a double and exact only to 53 bits, so a packed
`{generation, index}` handle can silently truncate `generation` once a slot has
been reused enough times.

**`FieldKind::Int` accepts any Lua number**, tested with `get_type() ==
sol::type::number` rather than `is<int32_t>()`. That check's strictness is
coupled to the Lua version, and neither variant of it means "is this usable as an
int".

**`world:each`'s `RuntimeQuery` is cached per term list, as a `World` resource**,
so its lifetime matches the `World`'s. A process-wide cache could alias a reused
heap address across `World` instances, between Catch2 test cases for example.

**`OnStart` and an entity's first `OnUpdate` run in the same `PreUpdate` pass**,
before that phase's flush. A deferred `world:add` issued in `OnStart` is
therefore still pending at the first `OnUpdate`, and becomes visible from the
second.

**LuaJIT was measured, not assumed.** On this build, recursive `fib(32)` is 2.7x
faster than plain Lua, and a 50M-iteration `sin`/`cos` loop is 5.0x faster. The
two benchmarks bracket LuaJIT's bias toward loops over deep recursion.

LuaJIT has no CMake config package, only a `.pc` file, so it is found through
`pkg_check_modules(... IMPORTED_TARGET luajit)`. sol2 detects it via
`LUAJIT_VERSION` in its own headers, so nothing else in this module changed for
the swap.

**The reload poll is a plain accumulator at about one second, not a filesystem
watch.** Nothing else in the engine has async or watch infrastructure to build one
on, and re-running a chunk is cheap enough that polling costs nothing noticeable.

## State

*As of 2026-09-06.* Working: embedding, the full ECS binding, per-entity instance
dispatch with isolated errors and independent state, `CommandBuffer`-deferred
structural changes, and mtime-poll hot reload. All of it has been exercised end
to end against the running app. LuaJIT is the shipped backend.

Tests: `modules/script/tests/`, one file per binding group. The audio one runs
with no sound engine at all — `audio::Init` is never called there — so it covers
the resolve path, the argument checking and the promise that a script on an
entity with no audio keeps running, and nothing about what is audible. Much of the rest was
verified by hand rather than automated — independent per-entity state, a live
edit reaching a running instance, and a deliberate error thrown from inside a
`world:each` callback being caught and logged.

`ScriptRef.scriptName` exists so scene files can round-trip which script an
entity runs. `instanceRef` is this run's live state and is deliberately not
saved. Resolving the name back into an `instanceRef` after a scene load is
`ResolveSceneScripts`, a private pass in `app` that `App::LoadScene` runs, rather
than something this module or `scene` does.

## Backlog

1. **Cover the hand-verified behaviour.** `ScriptHost` and `ScriptSystem` need
   only a bare `World` and a `sol::state` — no window, no renderer. Binding
   round-trips, per-instance isolation, a deferred mutation issued from inside a
   callback, and the `Entity`-as-userdata precision case are all candidates.
2. **`world:each` builds a fresh Lua table per matched entity per term, every
   call**, even though the `RuntimeQuery` itself is now cached. Whether that
   allocation is worth avoiding is unmeasured.
3. **No script-facing default for a native component's field**, the way
   `NewInstanceData()` gives one for a script's own state. `world:add` with no
   fields table uses whatever `ComponentOps` registered, which is a C++-side
   decision a script cannot override per call site.
4. Confirm the LuaJIT and plain-Lua swap stays a `vcpkg.json` entry plus one
   CMake block, across a future sol2 upgrade that changes how it detects the
   flavour underneath.

## Changed

*2026-10-02* — **Input bindings read `InputState::ForGame`.** Was: `Get`, so a
script saw every press, including those that navigate an open pause menu. Now:
at rest while the game UI holds the input. Nothing else changes for a script.

*2026-09-05* — **Asset copying was fixed to run on an asset-only change.** Was: a
`POST_BUILD` command in `cmake/Assets.cmake` that shared its target's up-to-date
check, so ninja skipped the copy whenever nothing needed relinking. A script edit
would cook fine and then never reach the running executable's `cooked/`
directory, which made hot reload look broken. Now: a separate `OUTPUT`-based
custom command keyed off the cook stamp.

`Shaders.cmake` and `Fonts.cmake` still use the old pattern and probably have the
same gap.
