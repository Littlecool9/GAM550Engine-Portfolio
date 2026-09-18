engine_add_module(input)

target_link_libraries(engine_input
    PUBLIC mir::core
           mir::window
)

target_link_libraries(engine_input PRIVATE mir::serial)

target_sources(engine_input PRIVATE
    src/input/InputMap.cpp
    src/input/InputSystem.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
