# core

The ECS, logging and asserts, executable-relative paths, and the surface contract
between a window and a renderer. It knows nothing about windows, graphics APIs or
assets, and has to stay that way.

`mir::core` · depends nothing engine-side; `glm` (public, in component headers), `spdlog` (private) · API `modules/core/include/core/` · maintainer Sumin Park · reviewed 2026-09-12

## Model

Four pieces share one library because they share one property: no engine
dependencies.

`ecs/` is by far the largest. `log/` gives logging and asserts without putting a
logging library in a public header. `fs/` resolves paths against the executable
rather than the working directory, so a copied build tree still runs.
`platform/` declares `ISurfaceProvider`, which lets a renderer take a drawing
surface from something without knowing that it is a window.

### Engine mode

`EngineModeState`, a `World` resource, is `EngineMode{Edit, Play}` plus
`PlayState{Running, Paused}` as its own axis — Play has a running/paused
sub-state, Edit does not. Systems can check `IsSimulating(world)`/ `IsEditing`/`IsPlaying`/`IsPaused` to check if they need to run. The
raw setters (`SetEngineMode`/`SetPlayState`) only flip the flags — they carry no
World snapshot/restore of their own; see [scene.md](scene.md)'s `PlaySnapshot`
for that.

### Storage

Storage is archetype tables, and only that.

Entities with the same component set share an `Archetype`, which holds one
`ComponentColumn` per type. Adding or removing a component computes a new
signature, finds or creates the archetype for it, and moves the row. That move is
a `memcpy` per column, which is why components are POD. Removing a row swaps the
last row into the hole it left.

A component with no fields is a **tag**. It gets a signature bit and no column at
all, so `AddTag<T>` only moves the entity into the archetype carrying that bit.

### Resources

Some state belongs to a world but not to any entity — the scene graph, a camera,
an asset cache, a script VM. That state lives in `World`, keyed by type.

Resources are not components and carry none of the POD constraints. Nothing ever
relocates one, so a resource may own heap memory and have a destructor.

```cpp
world.EmplaceResource<HierarchyIndex>();
HierarchyIndex *index = world.TryResource<HierarchyIndex>();
```

### Access by name

Some callers have a name rather than a type — a script binding, an inspector, a
deserializer. Those go through `ComponentRegistry::Instance()`.

The registry maps a name to a `ComponentOps`, which carries get, has, add,
remove and the two deferred forms, and to a `FieldDesc` table for named values.

A script declares its own component through the same registry, and it lands in
the same archetype tables as a C++ one:

```cpp
constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}, {"speed", FieldKind::Float}};
const ComponentOps &health = ComponentRegistry::Instance().RegisterRuntime("Health", fields);
```

## API

| Type | Header | Role |
|---|---|---|
| `Entity` | `ecs/Entity.h` | `{index, generation}` handle, 8 bytes |
| `TypeId` | `ecs/TypeId.h` | Per-type `{seq, hash, name}` |
| `Signature` | `ecs/Signature.h` | 256-bit component set, the archetype key |
| `Archetype`, `ComponentColumn` | `ecs/Archetype.h` | Rows sharing a signature; one type's data in one |
| `World` | `ecs/World.h` | Owns entities, archetypes, resources |
| `HierarchyIndex` | `ecs/HierarchyIndex.h` | The scene graph, held as a resource |
| `Transform`, `WorldTransform` | `ecs/components/` | Authored local TRS; derived world matrix plus staleness stamps |
| `Query<Ts...>` | `ecs/Query.h` | Cached view over matching archetypes |
| `RuntimeQuery` | `ecs/RuntimeQuery.h` | The same walk for terms known only at runtime |
| `ComponentRegistry`, `FieldDesc` | `ecs/ComponentRegistry.h`, `ecs/ComponentFields.h` | Name to erased ops; one named, typed value |
| `CommandBuffer` | `ecs/CommandBuffer.h` | Deferred structural changes |
| `ISystem`, `SystemScheduler` | `ecs/System.h`, `ecs/SystemScheduler.h` | Per-frame work, and running it by phase |
| `ISurfaceProvider` | `platform/Surface.h` | Native handle plus dimensions |
| `LaunchProcess`, `QuoteCommandLineArgument` | `platform/Process.h` | Start a process and optionally wait for its exit code; Windows argument quoting |
| `ExecutableDir`, `ExecutablePath` | `fs/Paths.h` | The running executable's directory and full path |
| `InitLog`, `FlushLog`, `ShutdownLog` | `log/Log.h` | Logger lifetime; `ShutdownLog` releases the log file for another process |
| `EngineMode`, `PlayState`, `EngineModeState` | `ecs/EngineMode.h` | Edit/Play and Play's Running/Paused sub-state, as a `World` resource |

`TypeIdOf<T>()` returns three things. `seq` is process-local and is used directly
as a signature bit position. `hash` is FNV-1a of the name, stable across runs,
which is what serialisation uses. `name` is the type name itself.

That name is extracted from `__FUNCSIG__` or `__PRETTY_FUNCTION__` at compile
time and then normalised — keywords stripped, spaces collapsed, namespaces
removed — so every compiler produces the same string. Namespaces being stripped
is why component names have to be globally unique.

`GetOrCreateQuery<Ts...>(filters...)` returns a cached `Query&`, which
re-resolves its matching archetypes when `World::Generation()` changes. Filters
are `With`, `Without` and `Or`, and each `Or` is its own clause.

`RuntimeQuery` shares `detail::ArchetypeMatcher` with `Query`, so the match test
and the cache rule have one implementation between them. It hands its callback
`(Entity, std::span<void *const>)`, valid for that call only. Unlike `Query`, the
caller owns it.

## Rules

**Components are POD.** `MIR_ASSERT_COMPONENT` checks it at the declaration and
storage checks it again. Anything that owns memory belongs in a resource, with
the component holding a handle to it.

**Component names are globally unique.** A collision asserts in Debug and would
silently alias in Release. `ComponentRegistry` re-checks with `MIR_CHECK`,
because a name that comes from data can collide in a Release build too.

**No structural change during iteration.** An add moves the entity to another
table and swap-removes its old row, so an entity you have not visited yet slides
into a row you already passed and is skipped without any sign. `AddComponent`,
`RemoveComponent` and `DestroyEntity` assert if a query is walking. Use the
`CommandBuffer` instead, and call `World::IsIterating()` if you are writing a
binding layer that has to route between the two.

`CreateEntity` is exempt. A new entity lands in the empty archetype, so no table
that a walk is matching against moves.

**A resource destructor must not call back into `World`.** By then `~World` is
already destroying the resource map. Anything holding entity handles needs an
explicit shutdown call rather than destructor cleanup.

**Re-emplacing a resource replaces and destroys the old one**, which invalidates
cached pointers to that resource. Emplace during setup, not mid-frame.

Other resources keep their addresses. Each one lives in its own heap holder, so
systems can safely cache a pointer in `OnStart`.

**Scene structure is not in components.** Parent and child edges live in
`HierarchyIndex`, where `RemoveComponent` cannot reach them.

`SetParent` refuses rather than asserts. It returns `false` on a cycle, on
self-parenting, and on a chain that would pass `kMaxHierarchyDepth`. Asserts
compile out under `NDEBUG`, and a single cycle makes every upward walk in the
engine non-terminating, so the graph has to stay bounded and acyclic in every
build.

Destroying an entity destroys its subtree. That is the only policy — detaching a
subtree instead is not expressible today.

**`Transform` is written through its setters.** Each setter bumps a version that
`WorldTransform` compares against, so a raw write leaves every descendant stale
permanently. That is why the members are private, and why reflection over raw
field offsets does not work on this type.

`ResolveWorld` refreshes an entity and every stale ancestor before returning, so
a read is correct at any point in the frame.
`TransformPropagateSystem` resolves everything once in `PostUpdate`, which is a
batching optimisation rather than the correctness mechanism.

The cost is that `ResolveWorld` mutates on read, so it is neither `const` nor
thread-safe.

**At most 256 component types**, counting C++ and script-declared ones together.
The budget is charged per distinct name for the life of the process, so a script
reload costs nothing.

Overflow stops the process in every build, checked once at allocation and again
on use. The second check is what catches a C++ component that was never
registered and drew its `seq` straight from `TypeIdOf`. As an assert it would
reach `std::bitset::set` in Release and throw out of `World::AddComponent`, in a
codebase that does not use exceptions.

**Erased operations are total.** `ComponentOps` answers for a dead entity instead
of asserting. The typed API asserts because touching a destroyed entity is a bug
in engine code, but a script holding a handle across the frame that destroyed it
is ordinary.

**A component is reachable by name only once registered**, and registration is
explicit rather than a static initialiser. `engine_core` is a static library, so
a self-registering object in a translation unit that nothing references gets
dropped by the linker, and the component would be present in Debug and missing in
Release.

**`FieldDesc` uses accessor thunks, not offsets**, for any component with an
invariant. An offset write to `mPosition` moves the object and leaves `mVersion`
untouched, which produces no crash and no assert, just one wrong frame in
everything downstream. Script-declared components have no invariant to protect
and are offset-backed.

**A stale `Entity` is detected, not honoured.** `GetComponent` returns `nullptr`
for a dead handle. `AddComponent` and the rest assert.

**Systems register before `Start`.** `Start` flushes at every phase boundary, so
an entity spawned in one `OnStart` is visible to the next phase's `OnStart`.
`OnStop` runs in reverse order.

**`World` is neither copyable nor movable.** Archetypes hold pointers back into
it.

**`ForEachAliveEntity` walks every live entity, not a query's matched rows.**
It exists for whole-World operations that cannot assume anything about which
components are present — `PlaySnapshot`'s capture/restore ([scene.md](scene.md))
is the one caller. A `Query` would miss any entity outside the types it was
built for; this walks `EntityPool` directly instead.

**Logging is initialised in `main`, outside `App`**, so a construction failure
inside `App` is still visible. `Trace` and `Debug` compile out in Release and are
free to leave in hot paths.

## State

*As of 2026-09-06.* Working: entities, archetype storage with tags, filtered and
runtime queries, deferred structural change, phase scheduling, typed resources,
the transform hierarchy with lazy world-matrix resolution, the component registry
with named field access, and script-declared component types.

Tests in `modules/core/tests/`: `Archetype`, `Query`, `CommandBuffer`,
`Resources`, `Transform`, `TransformHierarchy`, `System`, `ComponentRegistry`,
`RuntimeQuery`, `Paths`, `Process`.

Logging and asserts have no tests of their own. Every other test exercises them,
and their failure mode is loud.

`EngineModeState` and `ForEachAliveEntity` have no tests in this module. The
former is flag-flipping with nothing to assert on in isolation; the latter is
exercised by `scene`'s `PlaySnapshot.tests.cpp`, its one real caller.

Missing: parallelism, dependency ordering, change detection, component lifecycle
hooks, and row migration when a script component's layout changes.

## Backlog

1. **Row migration for a changed script component.** Changing a script
   component's field list needs the world restarted today, and the error message
   says so. The fix is to rebuild each holding archetype's column field by field,
   matching on name.
2. **Change detection.** Nothing tells a renderer which transforms moved this
   frame, though `WorldTransform::Version` already carries the information.
3. **Declared access for resources.** A parallel scheduler needs each system's
   read and write set. Resources are invisible to the query machinery that would
   otherwise report it, so either resources gain declared access, or systems that
   touch them stay serial.
4. **Reparenting is O(children)**, because stable child order rules out
   swap-and-pop erase. Tombstones plus a stored index would make it O(1). Waiting
   on a profile.
5. Undecided: whether systems should be forbidden from calling
   `World::AddComponent` directly, rather than it being convention.

## Changed

*2026-09-14* — **Starting processes, and handing the log file over.** Was: `core`
could say where the executable lives but not start another one, and the log file
stayed open until the process exited. Now: `ExecutablePath`,
`LaunchProcess` with Windows argument quoting in `platform/Process.h`, and
`ShutdownLog`. The editor's Open Project restarts into another game with them,
because the component registry and the type-id counter are process-wide and a
new process is the only clean slate.

*2026-09-12* — **`EngineModeState` and `World::ForEachAliveEntity` added.** Was:
no notion of Edit vs. Play in the ECS itself, and no way to enumerate every
entity independent of a query's component filter. Now: `EngineMode.h`'s
resource plus free-function readers, and `ForEachAliveEntity`, both driven by
the Play/Pause feature — a Play session needs to know it's running, and
stopping one needs to destroy every entity Play touched, including ones no
`LoadedScene` or query happens to track.

*2026-09-05* — **Sparse sets dropped; a fieldless component is a tag.** Was: two
storage models, opted into per type with `MIR_COMPONENT_SPARSE`. Now: archetype
tables only, with `kIsTagComponent<T> = std::is_empty_v<T>`.

Sparse was invisible to the typed API and refused by six erased paths, and the
erased surface is the one being grown. It had no callers outside tests. Its
intended use was empty tags on few entities, which is the case it handles worst:
with a signature bit, `Query<Transform>().With<Player>()` rejects a 99,995-row
table outright, and a sparse component owns no bit, so nothing can be rejected.
Removing it made `Or` accept a tag and let scripts declare tags, neither of which
needed new code.

Accepted knowingly: tags multiply archetype count, approaching 2^T for T
independently combining tags.

*2026-09-02* — **Scene structure moved out of components into a resource.** Was:
an intrusive POD chain, `Hierarchy{parent, firstChild, nextSibling, prevSibling}`.
Now: `HierarchyIndex`, a `World` resource with private mutators.

The chain was correct in isolation and failed repeatedly in place.
`RemoveComponent<Hierarchy>` on a mid-chain node truncated the parent's child
walk and dropped every later sibling silently, and no hook can intercept a
template that deletes invariants it has never heard of. The redundancy was never
the problem; storing it where the generic ECS could independently mutate it was.

*2026-09-02* — **`kMaxComponentTypes` raised from 128 to 256**, when scripts
gained the ability to declare component types and the set stopped being something
an author could count. It stays a static bitset: `Signature` is a by-value key
copied on every archetype transition, so heap-allocating it would cost far more
than the extra 16 bytes.
