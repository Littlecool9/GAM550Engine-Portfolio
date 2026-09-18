engine_add_module(serial)

find_package(nlohmann_json CONFIG REQUIRED)

target_sources(engine_serial PRIVATE
    src/serial/JsonFile.cpp
)

target_link_libraries(engine_serial
    PUBLIC mir::core
           nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
