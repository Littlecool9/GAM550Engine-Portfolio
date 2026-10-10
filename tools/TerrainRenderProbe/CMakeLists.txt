if(BUILD_TESTING)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(Stb REQUIRED)
    find_package(glfw3 CONFIG REQUIRED)
    add_executable(TerrainRenderProbe main.cpp RasterDepthEnvelope.cpp CullProbe.cpp FrameRegressionProbe.cpp RegionProbe.cpp
        SelectionProbe.cpp)
    target_include_directories(TerrainRenderProbe PRIVATE ${Stb_INCLUDE_DIR})
    target_link_libraries(TerrainRenderProbe PRIVATE terrain_bake mir::renderer mir::window nlohmann_json::nlohmann_json glfw)
    target_compile_definitions(TerrainRenderProbe PRIVATE TERRAIN_PROBE_RECIPE="${CMAKE_SOURCE_DIR}/tools/TerrainTool/tests/fixtures/mixed-2x2.terrain.json"
        TERRAIN_REGION_RECIPE="${CMAKE_SOURCE_DIR}/tools/TerrainTool/experiments/region-prototype.terrain.json"
        TERRAIN_CHECK_STREAMING="${CMAKE_SOURCE_DIR}/games/TerrainCheck/terrain_streaming.json")
    set_target_properties(TerrainRenderProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
