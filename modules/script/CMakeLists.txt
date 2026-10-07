engine_add_module(script)

find_package(sol2 CONFIG REQUIRED)

# luajit has no CMake config package - just a .pc file - so it's discovered
# through pkg-config instead of find_package(). sol2 auto-detects LuaJIT via
# LUAJIT_VERSION in its own headers; nothing else here needs to know which
# Lua flavor is underneath.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LuaJIT REQUIRED IMPORTED_TARGET luajit)

target_link_libraries(engine_script
    PUBLIC  mir::core
    PRIVATE mir::assets
            mir::audio
            mir::input
            mir::physics
            mir::terrain
            sol2::sol2
            PkgConfig::LuaJIT
)

target_sources(engine_script PRIVATE
    src/script/ComponentRegistration.cpp
    src/script/ScriptHost.cpp
    src/script/ScriptReloadWatcher.cpp
    src/script/ScriptSystem.cpp
    src/script/bindings/AudioBindings.cpp
    src/script/bindings/EntityBindings.cpp
    src/script/bindings/InputBindings.cpp
    src/script/bindings/PhysicsBindings.cpp
    src/script/bindings/TerrainBindings.cpp
    src/script/bindings/WorldBindings.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
