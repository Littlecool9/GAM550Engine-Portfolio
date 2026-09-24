engine_add_module(animation)
target_sources(engine_animation PRIVATE
    src/animation/ModelAnimation.cpp
    src/animation/ModelSystem.cpp
)
target_link_libraries(engine_animation PUBLIC mir::assets mir::core mir::renderer)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
