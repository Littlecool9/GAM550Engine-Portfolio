engine_add_module(physics)

target_link_libraries(engine_physics PUBLIC mir::core)

find_package(Jolt CONFIG REQUIRED)

# Jolt's precision has to match MIR_LARGE_WORLD: the root CMakeLists.txt asks
# vcpkg for joltphysics[double-precision] (vcpkg-overlays/joltphysics) through
# the large-world manifest feature. A mismatched Jolt would still link and only
# fail at runtime, in RegisterTypes.
get_target_property(_jolt_definitions Jolt::Jolt INTERFACE_COMPILE_DEFINITIONS)
if("${_jolt_definitions}" MATCHES "JPH_DOUBLE_PRECISION")
    set(_jolt_double ON)
else()
    set(_jolt_double OFF)
endif()
if((MIR_LARGE_WORLD AND NOT _jolt_double) OR (NOT MIR_LARGE_WORLD AND _jolt_double))
    message(FATAL_ERROR
        "Jolt::Jolt has JPH_DOUBLE_PRECISION=${_jolt_double} but MIR_LARGE_WORLD=${MIR_LARGE_WORLD}. "
        "VCPKG_MANIFEST_FEATURES must contain large-world exactly when MIR_LARGE_WORLD is ON "
        "(the root CMakeLists.txt sets it), and vcpkg must pick up vcpkg-overlays/; reconfigure.")
endif()

target_link_libraries(engine_physics PRIVATE Jolt::Jolt)

target_sources(engine_physics PRIVATE
    src/physics/CollisionEventSystem.cpp
    src/physics/ComponentRegistration.cpp
    src/physics/JoltRuntime.cpp
    src/physics/PhysicsSystem.cpp
    src/physics/PhysicsWorld.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
