cmake_minimum_required(VERSION 3.26)

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
add_subdirectory(modules/input)
add_subdirectory(modules/physics)
add_subdirectory(modules/renderer)
add_subdirectory(modules/scene)
add_subdirectory(modules/script)
add_subdirectory(modules/window)

add_subdirectory(tools/AssetCooker)

add_executable(MjolnirEngine WIN32)
target_sources(MjolnirEngine PRIVATE main.cpp)

if(MSVC)
    set_target_properties(MjolnirEngine PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/triangle.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/unlit.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/model.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

# Every games/*/ with a game.json is a game; its assetsRoot gets cooked
file(GLOB game_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/games/*/game.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${game_manifests})

set(game_names "")
set(game_asset_roots "")
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

    # skip missing roots (AssetCooker fails on them); ABSOLUTE normalizes "./"
    get_filename_component(assets_abs "${game_dir}/${assets_root}" ABSOLUTE)
    if(IS_DIRECTORY "${assets_abs}")
        file(RELATIVE_PATH assets_rel "${CMAKE_SOURCE_DIR}" "${assets_abs}")
        list(APPEND game_asset_roots "${assets_rel}")
    endif()
endforeach()

engine_cook_assets(MjolnirEngine
    SOURCE_ROOTS assets/ ${game_asset_roots}
    OUT_DIR ${CMAKE_BINARY_DIR}/cooked_assets)

engine_copy_fonts(MjolnirEngine SOURCE_DIR fonts)
engine_copy_branding(MjolnirEngine SOURCE_DIR branding)

# Not POST_BUILD, which only runs on relink; unchanged files are skipped
add_custom_target(MjolnirEngine_copy_games
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/games" "$<TARGET_FILE_DIR:MjolnirEngine>/games"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/engine.json" "$<TARGET_FILE_DIR:MjolnirEngine>/engine.json"
    COMMENT "Copying game projects and engine.json next to MjolnirEngine"
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
    nlohmann_json::nlohmann_json
)

# Smoke tests run each game for a few frames and fail on any logged error.
# They need a GPU; skip them with `ctest -LE smoke`.
if(BUILD_TESTING)
    add_executable(gltf_lifecycle_smoke tools/AssetCooker/tests/ModelLifecycleSmoke.cpp)
    target_link_libraries(gltf_lifecycle_smoke PRIVATE mir::app mir::animation)
    add_dependencies(gltf_lifecycle_smoke MjolnirEngine)
    add_test(NAME smoke.gltf_lifecycle COMMAND gltf_lifecycle_smoke)
    set_tests_properties(smoke.gltf_lifecycle PROPERTIES LABELS smoke TIMEOUT 120)
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
