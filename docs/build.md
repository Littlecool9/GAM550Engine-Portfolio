# Build

Windows only. Linux sources exist for the window and surface backends, but there
is no Linux preset.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | >= 3.26 | |
| Ninja | recent | Ships with the VS "C++ CMake tools" component |
| MSVC | VS 2026 or Build Tools | C++20 |
| Vulkan SDK | >= 1.4.309 | Also supplies `slangc`, required for shaders |
| vcpkg | any bootstrapped clone | Not vendored |
| VS Code | - | `ms-vscode.cmake-tools`, `ms-vscode.cpptools` |

## First-time setup

Steps 1–3 are once per machine; step 4 is once per clone.

### 1. vcpkg

```
git clone https://github.com/microsoft/vcpkg C:/dev/vcpkg
C:/dev/vcpkg/bootstrap-vcpkg.bat
setx VCPKG_ROOT C:/dev/vcpkg
```

Then **close every terminal and quit VS Code entirely**.

Do not vendor a second copy into this repo. One clone per machine means one
shared binary cache in `%LOCALAPPDATA%\vcpkg`, so only your first configure
anywhere pays the build cost.

### 2. Vulkan SDK

Install it from [vulkan.lunarg.com](https://vulkan.lunarg.com/sdk/home), version
1.4.309 or newer. The installer will set `VULKAN_SDK` env_path, and the
build reads `slangc` out of `$VULKAN_SDK/Bin`.

Restart terminals and VS Code again if they were open during the install.

### 3. Setting name in headers (optional)

```
setx MJOLNIR_AUTHOR "Your Name"
setx MJOLNIR_ORG "DigiPen (USA) Corporation"
```

Skip it and the scaffolding falls back to `git config user.name`, then the OS
user.

### 4. Clone and build

```
git clone --recurse-submodules <repo-url> C:/dev/MitosisEngine
cd C:/dev/MitosisEngine
cmake --preset windows-msvc-debug
```

From a Developer PowerShell, or a plain one after running `vcvars64.bat`.

This first configure installs every dependency and takes a few minutes. Later
ones are fast. If you already cloned without `--recurse-submodules`, run
`git submodule update --init --recursive` before configuring.

Then build and check it: [Build loop](#build-loop) and [Verify](#verify).

## Build loop

Every build is the same three steps.

1. Choose a preset. Skippable when you are reusing the previous selection, which
   is the usual case.
2. Configure.
3. Build.

`CMakePresets.json` is the only supported way to configure. There are two
presets, `windows-msvc-debug` and `windows-msvc-release`. Both use Ninja and both
build into `builds/<presetName>/`.

The command line and VS Code read the same presets, so everyone builds the same
way.

VS Code:

1. `Ctrl+Shift+P` → **CMake: Select Configure Preset** → `Debug` / `Release`
2. `F7` to build
3. `Ctrl+Shift+P` → **CMake: Set Launch/Debug Target** → `MjolnirEngine`
4. `Ctrl+F5` to run, `F5` to debug

After the first time, `F7` then `F5`.

With no arguments, `MjolnirEngine` loads the game named by `defaultGame` in
`engine.json`. To run another one, pass `--game games/<name>`, for example
through `args` in `.vscode/launch.json`.

Terminal, from a Developer PowerShell (or after `vcvars64.bat`):

```
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir builds/windows-msvc-debug --output-on-failure
```

Output goes to `builds/<presetName>/`, which is gitignored. Build into that tree
rather than making a second one.

Dependencies are declared in `vcpkg.json` and installed during configure. They
resolve against a pinned `builtin-baseline`, so everyone gets the same versions.

**Reconfigure** every time a `CMakeLists.txt` or `vcpkg.json` changes.

Ninja does it for you at build time, and cmake-tools can do it on save — see
`cmake.configureOnEdit`. To force one: `Ctrl+Shift+P` → **CMake: Configure**.

## Tests

### Run every test

Build first. Tests are built by the ordinary build, so `F7` or
`cmake --build` is all the preparation there is.

Terminal:

```
cmake --build --preset {selected_preset}
ctest --test-dir builds/{selected_preset} --output-on-failure
```

VS Code: `Ctrl+Shift+P` → **CMake: Run Tests**, or `F7` then the flask icon in
the sidebar to get the Testing panel.

The Testing panel is empty until the first build, because test cases are
discovered after the executables link. Build once and they appear.

Run the full `ctest` before pushing.

### Run one module, or one test

```
cmake --build builds/windows-msvc-debug --target engine_core_tests
ctest --test-dir builds/windows-msvc-debug -R "archetype" --output-on-failure
```

`--target` picks the executable, and the targets are named
`engine_<module>_tests`. `-R` then filters by Catch2 test case name, and takes a
regex, so `-R "archetype"` runs every case with that word in its name.

In the Testing panel the same thing is a click on one case or one file.

### Smoke tests

Every game with a `game.json` gets a `smoke.<game>` test. It runs
`MjolnirEngine --game games/<game> --frames 120`, which closes the window after
120 frames and exits non-zero if anything was logged as an error.

`smoke.switch` opens HelloWorld, switches to EmptyScene and back through
`App::OpenProject` — what Project > Open Project does, minus the dialog. The
switches come from `--switch-to <dir>`, which can repeat and needs `--frames`;
they are spread evenly across the run.

They open a real window, so they need a GPU and a display. `-L smoke` runs only
them, `-LE smoke` skips them:

```
ctest --test-dir builds/windows-msvc-debug -L smoke --output-on-failure
ctest --test-dir builds/windows-msvc-debug -LE smoke --output-on-failure
```

### How it is wired

Catch2 v3, one test executable per module, built under the `BUILD_TESTING`
option. That option is `ON` by default, which is why a plain build produces them.

`engine_add_module_tests` calls `catch_discover_tests`, so every `TEST_CASE`
becomes its own CTest case and a failure names the behaviour that broke.

`core`, `assets`, `input`, `scene`, `script` and `window` have tests. `renderer`
and `editor` need a device and a display, and are verified by running the engine,
which the smoke tests do once per game. The root CMakeLists registers those with
plain `add_test`, one per `games/*/game.json`.

There is no aggregate `tests` target. To build the test executables without the
games, list them:

```
cmake --build builds/windows-msvc-debug --target engine_core_tests engine_assets_tests engine_input_tests engine_scene_tests engine_script_tests engine_window_tests
```

To drop tests from the build entirely:

```
cmake --preset windows-msvc-debug -DBUILD_TESTING=OFF
```

## Making New Files

We have scripts for creating new files and modules.

Both are VS Code tasks: `Ctrl+Shift+P` → **Tasks: Run Task** → **New Module** or
**New File**. Both are PowerShell, so the convenience is Windows-only.

More info: [extending.md](extending.md).

## Verify

`MjolnirEngine` opens HelloWorld and renders the scene, `ctest` is all green, and
`logs/engine.log` has trace output.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `VCPKG_ROOT is not set` | `setx` only affects new processes. Restart the terminal and all of VS Code. |
| `slangc not found` | No Vulkan SDK, or `VULKAN_SDK` unset. |
| Configure fails right after **New Module** | A module with no sources is an empty library. Add a file. |
| A new `.cpp` never links | Nothing is globbed. List it in its module's `target_sources`. |
| A new asset file is not cooked | A new *file* needs a reconfigure to join the glob; editing an existing one does not. |
| IntelliSense misses a new dependency | **CMake: Delete Cache and Reconfigure**. |
| A header gets no IntelliSense at all | `C_Cpp.intelliSenseEngineFallback` is off, so a header no compiled `.cpp` reaches gets nothing. Include it from a test. |
| `cl.exe` not found in a plain terminal | MSVC environment not loaded. Use a Developer PowerShell or prefix with `vcvars64.bat`. |

## Changed

*2026-09-01* — **CMake, not premake.** Premake is pleasanter to write, but
vcpkg, CMake presets and the VS Code tooling all assume CMake, and the ecosystem
around it is not close.

Single-config generators were chosen over multi-config, because single-config
keeps a configuration component out of the output paths, which the shader and
asset copy steps depend on.
