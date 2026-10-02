cmake_minimum_required(VERSION 3.26)


option(MIR_LARGE_WORLD "Double-precision world positions and Jolt" ON)
if(MIR_LARGE_WORLD)
    list(APPEND VCPKG_MANIFEST_FEATURES large-world)
endif()

project(Mjolnir
    VERSION 0.1.0
    DESCRIPTION "A modular 3D engine"
    # C is needed for volk.c
    LANGUAGES CXX C)

# C++ 20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

# Editing the manifest must re-run configure, otherwise vcpkg never installs
# the new dependency and IntelliSense keeps the stale include paths.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json")

#line for wwise
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#end
include(cmake/EnginePlatform.cmake)
include(cmake/EngineModule.cmake)
include(cmake/Shaders.cmake)
include(cmake/Assets.cmake)
include(cmake/Fonts.cmake)
include(cmake/Branding.cmake)

option(BUILD_TESTING "Build unit tests (per module, run via ctest)" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

add_subdirectory(third_party)
add_subdirectory(modules/audio)
add_subdirectory(modules/app)
add_subdirectory(modules/assets)
add_subdirectory(modules/animation)
add_subdirectory(modules/core)
add_subdirectory(modules/editor)
add_subdirectory(modules/editortheme)
add_subdirectory(modules/gameui)
add_subdirectory(modules/imguiruntime)
add_subdirectory(modules/input)
add_subdirectory(modules/physics)
add_subdirectory(modules/renderer)
add_subdirectory(modules/scene)
add_subdirectory(modules/serial)
add_subdirectory(modules/script)
add_subdirectory(modules/terrain)
add_subdirectory(modules/window)

add_subdirectory(tools/AssetCooker)

add_executable(MjolnirEngine WIN32)
target_sources(MjolnirEngine PRIVATE main.cpp)

# Local development binding used by the editor's one-shot import workflow.
# It is generated into the build tree and never written into game data.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" asset_workspace_source_root)
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:MjolnirEngine>/asset_workspace.json"
    CONTENT "{\n  \"version\": 1,\n  \"sourceRoot\": \"${asset_workspace_source_root}\",\n  \"assetCooker\": \"$<TARGET_FILE:AssetCooker>\"\n}\n")

if(MSVC)
    set_target_properties(MjolnirEngine PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/default.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/unlit.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/model.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/sky.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/shadowDepth.slang
    ENTRIES vertexMain:vertex)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/shadowDistance.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/raytrace.slang
    ENTRIES rayGenMain:raygeneration missMain:miss closestHitMain:closesthit shadowMissMain:miss)

# Every games/*/ with a game.json is a game; its assetsRoot gets cooked
file(GLOB game_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/games/*/game.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${game_manifests})

set(game_names "")
set(game_asset_roots "")
set(game_copy_commands "")
foreach(manifest IN LISTS game_manifests)
    get_filename_component(game_dir "${manifest}" DIRECTORY)
    get_filename_component(game_name "${game_dir}" NAME)
    list(APPEND game_names "${game_name}")

    file(READ "${manifest}" game_json)
    string(JSON game_json_length ERROR_VARIABLE game_json_error LENGTH "${game_json}")
    if(game_json_error)
        message(WARNING "${manifest} is not valid JSON (${game_json_error}), cooking its default assets root")
    endif()

    # default must match LoadGameProject in app/GameProject.cpp
    string(JSON assets_root ERROR_VARIABLE assets_root_error GET "${game_json}" assetsRoot)
    if(assets_root_error)
        set(assets_root "assets")
    endif()

    # The id root LoadGameProject builds: "games/<dir>/" + assetsRoot normalized on
    # its own, so a root it refuses (outside the game, ".", absolute) is not cooked.
    cmake_path(SET assets_norm NORMALIZE "${assets_root}")
    string(REGEX REPLACE "/+$" "" assets_norm "${assets_norm}")
    if(assets_norm STREQUAL "" OR assets_norm STREQUAL "." OR assets_norm MATCHES "^\\.\\.(/|$)"
       OR IS_ABSOLUTE "${assets_root}" OR assets_root MATCHES "^[/\\\\]|:")
        message(WARNING "${manifest}: assetsRoot \"${assets_root}\" is not a relative path inside the game; not cooked")
    else()
        set(assets_rel "games/${game_name}/${assets_norm}")
        # skip missing roots (AssetCooker fails on them)
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${assets_rel}")
            list(APPEND game_asset_roots "${assets_rel}")
            list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                 "${CMAKE_SOURCE_DIR}/${assets_rel}" "$<TARGET_FILE_DIR:MjolnirEngine>/${assets_rel}")
        endif()
    endif()

    list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
         "${manifest}" "$<TARGET_FILE_DIR:MjolnirEngine>/games/${game_name}/game.json")

    set(scene_dest "${CMAKE_BINARY_DIR}/games/${game_name}/scenes")
    if(IS_DIRECTORY "${game_dir}/scenes" AND NOT EXISTS "${scene_dest}")
        file(COPY "${game_dir}/scenes" DESTINATION "${CMAKE_BINARY_DIR}/games/${game_name}")
    endif()

    set(settings_src "${game_dir}/project_settings.json")
    set(settings_dest "${CMAKE_BINARY_DIR}/games/${game_name}/project_settings.json")
    if(EXISTS "${settings_src}" AND NOT EXISTS "${settings_dest}")
        file(COPY "${settings_src}" DESTINATION "${CMAKE_BINARY_DIR}/games/${game_name}")
    endif()
endforeach()

engine_cook_assets(MjolnirEngine
    SOURCE_ROOTS assets/ ${game_asset_roots}
    OUT_DIR ${CMAKE_BINARY_DIR}/cooked_assets)

engine_copy_fonts(MjolnirEngine SOURCE_DIR fonts)
engine_copy_branding(MjolnirEngine SOURCE_DIR branding)

add_custom_target(MjolnirEngine_copy_games
    ${game_copy_commands}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/engine.json" "$<TARGET_FILE_DIR:MjolnirEngine>/engine.json"
    COMMENT "Copying game assets/manifests and engine.json next to MjolnirEngine"
    VERBATIM)
add_dependencies(MjolnirEngine MjolnirEngine_copy_games)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(MjolnirEngine PRIVATE
    mir::app
    mir::assets
    mir::audio
    mir::core
    mir::input
    mir::physics
    mir::scene
    mir::script
    mir::terrain
    nlohmann_json::nlohmann_json
)

# Smoke tests run each game for a few frames and fail on any logged error.
# They need a GPU; skip them with `ctest -LE smoke`.
if(BUILD_TESTING)
    foreach(game_name IN LISTS game_names)
        add_test(NAME smoke.${game_name}
            COMMAND MjolnirEngine --game games/${game_name} --frames 120)
        set_tests_properties(smoke.${game_name} PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endforeach()

    # Restarts HelloWorld -> EmptyScene -> HelloWorld, as Open Project does
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/HelloWorld/game.json"
       AND EXISTS "${CMAKE_SOURCE_DIR}/games/EmptyScene/game.json")
        add_test(NAME smoke.switch
            COMMAND MjolnirEngine --game games/HelloWorld
                    --switch-to games/EmptyScene --switch-to games/HelloWorld
                    --frames 180)
        set_tests_properties(smoke.switch PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()
endif()
