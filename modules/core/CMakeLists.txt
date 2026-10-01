
engine_add_module(core)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core/EngineVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/core/EngineVersion.h
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core/WorldConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/core/WorldConfig.h
    @ONLY)
target_include_directories(engine_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated_include)

target_sources(engine_core PRIVATE
    src/core/log/Log.cpp
    src/core/log/LogHistory.cpp
    src/core/fs/Paths.cpp
    src/core/fs/FileIO.cpp
    src/core/platform/Process.cpp
    src/core/ecs/TransformHierarchy.cpp
    src/core/ecs/HierarchyIndex.cpp
    src/core/ecs/ComponentRegistry.cpp
    src/core/ecs/EntityPresetRegistry.cpp
    src/core/ecs/RuntimeQuery.cpp
    src/core/ecs/DeferredAccess.cpp)

# get external libraries here
find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# spdlog stays private
target_link_libraries(engine_core PRIVATE spdlog::spdlog)

# glm is PUBLIC: glm types are in the public component headers, so anything
# that includes core/ecs/components/Transform.h needs glm's include path too
target_link_libraries(engine_core PUBLIC glm::glm)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()