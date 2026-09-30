engine_add_module(physics)

target_link_libraries(engine_physics PUBLIC mir::core)

find_package(Jolt CONFIG REQUIRED)

# World positions are double end to end, so Jolt has to be the double-precision
# build from vcpkg-overlays/joltphysics. A float Jolt would still link and only
# fail at runtime, in RegisterTypes.
get_target_property(_jolt_definitions Jolt::Jolt INTERFACE_COMPILE_DEFINITIONS)
if(NOT "${_jolt_definitions}" MATCHES "JPH_DOUBLE_PRECISION")
    message(FATAL_ERROR
        "Jolt::Jolt was built without JPH_DOUBLE_PRECISION. The engine needs the "
        "joltphysics port in vcpkg-overlays/, which vcpkg-configuration.json registers; "
        "check that vcpkg picked the overlay up, then reconfigure.")
endif()

target_link_libraries(engine_physics PRIVATE Jolt::Jolt)

target_sources(engine_physics PRIVATE
    src/physics/ComponentRegistration.cpp
    src/physics/JoltRuntime.cpp
    src/physics/PhysicsSystem.cpp
    src/physics/PhysicsWorld.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
