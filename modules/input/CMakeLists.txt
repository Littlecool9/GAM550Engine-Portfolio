engine_add_module(input)

target_link_libraries(engine_input
    PUBLIC mir::core
           mir::window
)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(engine_input PRIVATE nlohmann_json::nlohmann_json)

target_sources(engine_input PRIVATE
    src/input/InputMap.cpp
    src/input/InputSystem.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
