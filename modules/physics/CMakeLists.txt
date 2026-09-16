engine_add_module(physics)

target_link_libraries(engine_physics PUBLIC mir::core)

# Jolt stays private: no JPH header is reachable from include/physics, so
# nothing downstream picks up Jolt's include path or its JPH_* definitions.
find_package(Jolt CONFIG REQUIRED)
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
