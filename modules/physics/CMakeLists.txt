engine_add_module(physics)

target_link_libraries(engine_physics PUBLIC mir::core)

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
