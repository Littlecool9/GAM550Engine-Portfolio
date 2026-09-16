# Mjolnir Engine

A modular 3D engine. It has an archetype ECS, a Vulkan 1.3 renderer, an offline
asset pipeline, a Lua scripting layer and an ImGui editor shell, and every part
of it is its own static library.

A game is data, not a build target. Each game is a directory under `games/`
holding configuration, assets, Lua scripts and scenes. One shared runtime loads
that directory, so changing a game needs no rebuild.

## Folder structure

```
CMakeLists.txt        root build: module list, MjolnirEngine target, game discovery
CMakePresets.json     Debug / Release, the only supported configure path
vcpkg.json            dependency manifest
engine.json           defaultGame, loaded when --game is not given
main.cpp              MjolnirEngine entry point: loads one game directory

cmake/                build helpers
  EngineModule.cmake    engine_add_module, engine_add_module_tests
  EnginePlatform.cmake  ENGINE_PLATFORM and family flags
  Shaders.cmake         slangc compilation
  Assets.cmake          asset cooking
  Fonts.cmake           font copying
  Branding.cmake        icon and splash copying

modules/              one static library each, mir::<name>
  core/                 ECS, logging, paths, the surface contract
  window/               GLFW window behind an interface
  renderer/             Vulkan 1.3 renderer, render components, RenderSystem
  assets/               cooked blobs, manifest, cache
  scene/                scene save and load
  script/               Lua embedding and ECS bindings
  audio/                Wwise lifetime and the per-frame pump
  editor/               ImGui context, backends, editor shell
  editortheme/          the Slate ImGui theme, data only
  app/                  composition root and main loop
    include/<name>/       public API — the whole surface of the module
    src/<name>/           implementation and private headers
    tests/                Catch2 tests, one CTest case per TEST_CASE

games/                one directory per game — no C++
  HelloWorld/
    game.json             title, assetsRoot, sceneDir, relative to the game
    assets/               meshes and Lua scripts, cooked separately
    scenes/               scene.json plus one file per entity
  EmptyScene/           a game with an empty scene, for the smoke test

tools/                AssetCooker, new_module.ps1, new_file.ps1
templates/            what the scaffolding scripts stamp out
third_party/          imgui (submodule), imgui_config, IconFontCppHeaders
assets/               engine source assets and shaders, cooked into the build tree
fonts/, branding/     copied next to the executable
docs/                 build, architecture, conventions, extending, per module
builds/, logs/        gitignored
```

How the modules depend on each other, and why:
[docs/architecture.md](docs/architecture.md#module-graph).

## Read next

| Document | Answers |
|---|---|
| [docs/build.md](docs/build.md) | How do I build, run and debug it? |
| [docs/architecture.md](docs/architecture.md) | What exists, how does it fit, why this shape? |
| [docs/conventions.md](docs/conventions.md) | How do I write code that fits in? |
| [docs/extending.md](docs/extending.md) | How do I add a module, component, system, shader, asset? |
| [docs/modules/](docs/modules/) | How does one module work? |

## Requirements

Windows only. CMake >= 3.26, Ninja, MSVC (VS 2026 or Build Tools), Vulkan SDK
>= 1.4.309, a bootstrapped vcpkg with `VCPKG_ROOT` set, and VS Code with
`ms-vscode.cmake-tools` + `ms-vscode.cpptools`.

## Quick start

```
git clone --recurse-submodules <repo-url>
git submodule update --init --recursive
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir builds/windows-msvc-debug --output-on-failure
```

Run those from a Developer PowerShell, or from a plain one after
`vcvars64.bat`. In VS Code, select the `windows-msvc-debug` configure preset,
then `F7` to build and `F5` to debug.

This assumes vcpkg and the Vulkan SDK are already set up. Full setup,
troubleshooting and the VS Code workflow are in [docs/build.md](docs/build.md).

## Third-party notices

Dear ImGui is a `third_party/imgui` submodule, MIT licensed, with `LICENSE.txt`
inside it.

`fonts/Inter.ttf` is Inter by Rasmus Andersson, SIL OFL, `fonts/Inter-OFL.txt`.

Icon glyphs are Font Awesome Free, `fonts/fa-solid-900.ttf`, licensed in
`fonts/FontAwesome-LICENSE.txt` and addressed through
`third_party/IconFontCppHeaders/IconsFontAwesome6.h`, zlib.

Everything else comes from vcpkg, through `vcpkg.json`.
