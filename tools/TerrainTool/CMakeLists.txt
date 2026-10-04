find_package(nlohmann_json CONFIG REQUIRED)

# Offline terrain pipeline; the engine never links it.
add_library(terrain_bake STATIC
    TerrainGenerator.cpp
    TerrainNoise.cpp
    TerrainRecipe.cpp
)
target_include_directories(terrain_bake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(terrain_bake
    PUBLIC mir::terrain mir::assets mir::core
    PRIVATE nlohmann_json::nlohmann_json)

if(BUILD_TESTING)
    add_executable(terrain_tool_tests
        tests/TerrainGenerator.tests.cpp
        tests/TerrainRecipe.tests.cpp
    )
    target_link_libraries(terrain_tool_tests PRIVATE terrain_bake nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    catch_discover_tests(terrain_tool_tests)
endif()
