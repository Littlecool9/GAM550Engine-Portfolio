# Extending

Recipes. The rules behind them are in [conventions.md](conventions.md).

## Add a module

Add one when a feature has an API that other modules use, and dependencies of its
own.

1. **Tasks: Run Task** → **New Module**, or
   `powershell -NoProfile -File tools/new_module.ps1 -Name input`
2. It creates `modules/<name>/` with `include/`, `src/` and a CMakeLists calling
   `engine_add_module(<name>)`, registers the subdirectory, and links
   `mir::<name>` into `MjolnirEngine`.
3. Fill in the CMakeLists with your sources and dependencies. The next section
   is a complete one.
4. Add a file before configuring. CMake cannot build a library with no sources,
   and the script tells you so.

Then check the [module graph](architecture.md#module-graph) still holds, add a
row to its table, and start a `docs/modules/<name>.md` from
[TEMPLATE.md](modules/TEMPLATE.md).

Without the scripts, do the same by hand: create
`modules/<name>/include/<name>/` and `src/<name>/`, write the CMakeLists below,
then add `add_subdirectory(modules/<name>)` and `mir::<name>` to the root
CMakeLists.

### A module's CMakeLists

This is every piece a module CMakeLists can have. Most modules use only the first
two.

```cmake
# modules/input/CMakeLists.txt

# Creates the target engine_input with the alias mir::input, and puts
# include/ on its public include path. Always the first line.
engine_add_module(input)

# Every .cpp that has to compile is listed here. 
# file left out of this list never reaches the link.
target_sources(engine_input PRIVATE
    src/input/InputSystem.cpp
    src/input/glfw/GLFWInput.cpp)

# Engine dependencies. PUBLIC because core's types appear in this module's
# own public headers, so anything using mir::input needs core's headers too.
target_link_libraries(engine_input PUBLIC mir::core)

# Third-party dependencies come from vcpkg.json, found by config package.
find_package(glfw3 CONFIG REQUIRED)

# PRIVATE because no GLFW type appears in include/input/. Consumers of
# mir::input never get GLFW's include path.
target_link_libraries(engine_input PRIVATE glfw)

# Only if the module has a tests/ directory.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

And the tests directory, if there is one:

```cmake
# modules/input/tests/CMakeLists.txt

# Builds engine_input_tests, links the module plus Catch2, and registers
# each TEST_CASE as its own CTest case.
engine_add_module_tests(input
    InputSystem.tests.cpp)
```

Inside its own CMakeLists a module is `engine_<name>`. Everywhere else it is the
alias `mir::<name>`.

Platform-specific sources go behind the platform flags from
`cmake/EnginePlatform.cmake`, the way `window` does it:

```cmake
if(ENGINE_FAMILY_DESKTOP)
    target_sources(engine_input PRIVATE src/input/glfw/GLFWInput.cpp)

    if(ENGINE_PLATFORM STREQUAL "Windows")
        target_sources(engine_input PRIVATE src/input/glfw/GLFWInputWin32.cpp)
    elseif(ENGINE_PLATFORM STREQUAL "Linux")
        target_sources(engine_input PRIVATE src/input/glfw/GLFWInputLinux.cpp)
    else()
        message(FATAL_ERROR "No input backend for ${ENGINE_PLATFORM}")
    endif()
endif()
```

The `else()` branch matters. Without it an unhandled platform produces a library
with no backend, which fails at link time rather than at configure time.

## Add a file

**Tasks: Run Task** → **New File**, or
`powershell -NoProfile -File tools/new_file.ps1 -Module core -Path ecs/Archetype`

`class` gives you a header plus a source file and lists the `.cpp` in
`target_sources`. `header` gives you the header alone.

Header-only is normal here, especially in `core/ecs`. Use `header` rather than
creating an empty `.cpp` to go with it.

## Add a component

```cpp
#include <core/ecs/ComponentAsserts.h>

namespace mir
{
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
    };
    MIR_ASSERT_COMPONENT(Position);

    // No fields: a tag. One signature bit, no column.
    struct Frozen
    {
    };
    MIR_ASSERT_COMPONENT(Frozen);
}
```

No constructor or destructor runs on a component, so defaults have to be member
initialisers.

The name must be globally unique across namespaces, because `TypeIdOf` hashes
the bare name.

Add a tag with `world.AddTag<Frozen>(entity)` and remove it with the usual
`RemoveComponent<Frozen>`. A tag has nothing to read, so `GetComponent` and a
`Query<...>` data term both reject it at compile time. Filter on it instead:

```cpp
world.GetOrCreateQuery<Position>(Without<Frozen>{})
    .ForEach([](Entity, Position &position) { /* ... */ });
```

To make a component reachable by name — from Lua, from the inspector, or from a
save file — register it with a field table in your module's
`ComponentRegistration.cpp`.

Use accessor thunks rather than raw offsets for any component that has an
invariant to protect. Writing `Transform::mPosition` through an offset moves the
object without touching `mVersion`, so every world matrix downstream keeps
looking current.

## Add a system

```cpp
class MovementSystem final : public ISystem
{
public:
    void OnStart(SystemContext &context) override
    {
        mQuery = &context.world.GetOrCreateQuery<Position, const Velocity>();
    }

    void OnUpdate(SystemContext &context) override
    {
        const float dt = context.dt;
        mQuery->ForEach([&](Entity entity, Position &position, const Velocity &velocity)
        {
            position.x += velocity.x * dt;

            // Structural change is recorded, never applied here.
            if (position.x > 100.0f)
                context.commands.Destroy(entity);
        });
    }

private:
    Query<Position, const Velocity> *mQuery = nullptr;
};
```

Register it before `Run`, in the phase that expresses the ordering you need:

```cpp
app.Systems().Add<MovementSystem>(SystemPhase::Update);
```

`Add`, `Remove` and `Destroy` go on `context.commands` and land at the next phase
boundary. Mutating inside `ForEach` invalidates the iteration.

An entity from `World::CreateEntity` has a handle immediately, but its components
only exist after the flush.

Systems cannot be registered after `Start`.

A system that installs a `World::AddDestroyHook` has to remove it in `OnStop`.
Left behind, the hook calls into freed memory on the next entity destroyed.

Filters go in the same call as the data terms:

```cpp
context.world.GetOrCreateQuery<Position>(Without<Frozen>{}, With<Active>{});
```

## Add tests

Create `modules/<name>/tests/<Subject>.tests.cpp` and list it:

```cmake
engine_add_module_tests(core
    Paths.tests.cpp
    Archetype.tests.cpp
)
```

If the module has no tests yet, gate the subdirectory in its CMakeLists with
`if(BUILD_TESTING)`.

## Add a shader

Write a `.slang` file under `assets/shaders/`, then declare it:

```cmake
engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/triangle.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)
```

You get one `.spv` per entry point, named `<source>.<entry>.spv`, copied next to
the executable. Load it with `mir::ShaderPath("...")`, which resolves against the
executable rather than the working directory.

`slangc` is looked up in `%VULKAN_SDK%\Bin` first, so the compiler matches the
installed validation layers, then on `PATH`, and configure fails if neither has
it. There is no glslc fallback, because a silent fallback would compile a
different language than the source is written in.

The flags are fixed. SPIR-V 1.5, column-major matrices to match GLM, entry-point
names preserved — without that flag Slang renames every entry point to `main` —
and `-g -O0` so RenderDoc shows Slang source.

The debug flags are unconditional rather than per-configuration. The output path
carries no configuration component, so a per-config flag would leave one
configuration reading the other's `.spv`.

## Add assets

Drop the file under a cooked source root. Building recooks it. A new *file* needs
a reconfigure to join the glob; editing an existing one does not.

```cpp
if (AssetCache *cache = app.Assets())
{
    if (const AssetBlobView *blob = cache->Load(MakeAssetId("assets/foo.txt")))
        Use(blob->content);
}
```

`Assets()` returns `nullptr` when there is no manifest. Handle that rather than
asserting on it.

A game's assets root is cooked without any of this, see
[Add a game](#add-a-game). Any other root joins `SOURCE_ROOTS` in the root
CMakeLists:

```cmake
engine_cook_assets(MjolnirEngine
    SOURCE_ROOTS assets/ ${game_asset_roots} tools/mytool/assets/
    OUT_DIR ${CMAKE_BINARY_DIR}/cooked_assets)
```

`SOURCE_ROOTS` has to be **relative to the repository root**. An absolute path is
rejected, because the root string is hashed into every asset id.

For a typed asset, add a type tag and a content version in `AssetBlob.h`, teach
`tools/AssetCooker/main.cpp` to recognise the extension and emit that tag, then
bump the content version whenever the layout changes. That bump is what forces a
recook instead of a silent mismatch at load.

## Add a game

A game is a directory under `games/` with a `game.json`. It needs no C++ and no
CMake:

```
games/mygame/
  game.json
  assets/              optional, cooked when present
  scenes/default/      optional, written by File → Save
```

```json
{
    "name": "mygame",
    "title": "MjolnirEngine - mygame",
    "assetsRoot": "assets",
    "sceneDir": "scenes/default"
}
```

`assetsRoot` and `sceneDir` are relative to the game directory, and default to
the values above. Input bindings save to `project_settings.json` next to
`game.json`. Scene files name meshes and scripts relative to `assetsRoot`, so
`meshes/cube.mesh.json` loads the asset whose id is the hash of
`games/mygame/assets/meshes/cube.mesh.json`.

Build. The root CMakeLists globs for `game.json`, so the build re-runs configure,
cooks the game's `assetsRoot`, copies the game next to the executable, and
registers a `smoke.mygame` test. Then run `MjolnirEngine --game games/mygame`,
or set `defaultGame` in `engine.json`.

A directory without `game.json` is refused at startup. A `sceneDir` with no
`scene.json` yet starts an empty scene.

## Add a script to an entity

Load the source, create an instance, and attach a `ScriptRef`:

```cpp
const AssetId id = MakeAssetId("games/HelloWorld/assets/scripts/spin.lua");
const AssetBlobView *blob = app.Assets()->Load(id);

app.Scripts().LoadScriptSource("spin", AsStringView(*blob));
app.ScriptReload().Track("spin", id);          // picks up an edit on the next cook

const int32_t instance = app.Scripts().CreateInstance("spin");
world.AddComponent<ScriptRef>(entity, ScriptRef{.instanceRef = instance});
```

A script defines `OnStart`, `OnUpdate` and `OnStop`, plus an optional
`NewInstanceData()` for its own per-entity state.

The bindings and a full example are in [modules/script.md](modules/script.md).

## Add a dependency

Add it to `vcpkg.json`, which re-runs configure on its own. Then
`find_package(... CONFIG REQUIRED)` in the module that needs it, and link
`PRIVATE` unless one of its types appears in your public header.

## Add a platform

1. Extend `cmake/EnginePlatform.cmake` if the platform is not detected.
2. Add a configure and build preset pair.
3. Implement `GLFW<Platform>.cpp` for the native handle and
   `VulkanSurface<Platform>.cpp` for the surface, plus the `VK_USE_PLATFORM_*`
   define.
4. Add the `WindowBackend` enumerator in `core/platform/Surface.h`.
5. Update [build.md](build.md).
