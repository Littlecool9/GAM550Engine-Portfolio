engine_add_module_tests(terrain
    TerrainAreaSamples.tests.cpp
    TerrainAssetPath.tests.cpp
    TerrainCollisionResidency.tests.cpp
    TerrainHeightQuery.tests.cpp
    TerrainIdentity.tests.cpp
    TerrainLayout.tests.cpp
    TerrainMeshResidency.tests.cpp
    TerrainPackFormat.tests.cpp
    TerrainPackIntegrity.tests.cpp
    TerrainRegion.tests.cpp
    TerrainRelease.tests.cpp
    TerrainReleaseSwitch.tests.cpp
    TerrainScene.tests.cpp
    TerrainStreamer.tests.cpp
    TerrainStreamingSettings.tests.cpp
    TerrainSurface.tests.cpp
    TerrainTileMesh.tests.cpp
    TerrainViewSelection.tests.cpp
    TileAddressing.tests.cpp
    TileHeights.tests.cpp
)

target_compile_definitions(engine_terrain_tests PRIVATE TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# TerrainInstance must round-trip SceneIO; terrain itself never links scene.
target_link_libraries(engine_terrain_tests PRIVATE mir::scene)
