engine_add_module(scene)

target_sources(engine_scene PRIVATE
    src/scene/SceneIO.cpp
    src/scene/PlaySnapshot.cpp)

# core's Entity/World types are in scene's public headers (SceneIO.h)
target_link_libraries(engine_scene PUBLIC mir::core)

# JSON only appears in SceneIO.cpp, never in a public header
target_link_libraries(engine_scene PRIVATE mir::serial)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
