find_package(nlohmann_json CONFIG REQUIRED)

# Offline terrain pipeline; the engine never links it.
add_library(terrain_bake STATIC
    TerrainBake.cpp
    TerrainCandidate.cpp
    TerrainContinuity.cpp
    TerrainDerivatives.cpp
    TerrainMixedContinuity.cpp
    TerrainGenerator.cpp
    TerrainNoise.cpp
    TerrainPackage.cpp
    TerrainPublish.cpp
    TerrainRecipe.cpp
    TerrainReport.cpp
    TerrainSeams.cpp
    TerrainSync.cpp
    TerrainValidate.cpp
)
target_include_directories(terrain_bake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(terrain_bake
    PUBLIC mir::terrain mir::assets mir::core
    PRIVATE nlohmann_json::nlohmann_json)

# The root CMakeLists runs its sync (and package without the editor) on every build.
add_executable(TerrainTool main.cpp)
target_link_libraries(TerrainTool PRIVATE terrain_bake)

if(BUILD_TESTING)
    add_executable(terrain_tool_tests
        tests/TerrainBake.tests.cpp
        tests/TerrainGenerator.tests.cpp
        tests/TerrainDerivatives.tests.cpp
        tests/TerrainPackage.tests.cpp
        tests/TerrainPrecision.tests.cpp
        tests/TerrainPublish.tests.cpp
        tests/TerrainRecipe.tests.cpp
        tests/TerrainRenderMesh.tests.cpp
        tests/TerrainSeams.tests.cpp
        tests/TerrainSync.tests.cpp
        tests/TerrainToolCli.tests.cpp
        tests/TerrainValidate.tests.cpp
    )
    target_link_libraries(terrain_tool_tests PRIVATE terrain_bake mir::renderer nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_TOOL_EXE="$<TARGET_FILE:TerrainTool>")
    add_dependencies(terrain_tool_tests TerrainTool)
    catch_discover_tests(terrain_tool_tests)
endif()

option(MJOLNIR_TERRAIN_INDEX_BENCHMARK "Build the metadata-only format 2 index benchmark" OFF)
if(MJOLNIR_TERRAIN_INDEX_BENCHMARK)
    add_executable(terrain_index_benchmark experiments/TerrainIndexBenchmark.cpp)
    target_link_libraries(terrain_index_benchmark PRIVATE mir::terrain)
endif()

option(MJOLNIR_TERRAIN_SEAM_EXPERIMENT "Build mixed-source transition measurements" OFF)
if(MJOLNIR_TERRAIN_SEAM_EXPERIMENT)
    add_executable(terrain_seam_experiment experiments/TerrainSeamExperiment.cpp)
    target_link_libraries(terrain_seam_experiment PRIVATE terrain_bake)
endif()
