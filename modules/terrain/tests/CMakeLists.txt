engine_add_module_tests(terrain
    MockTerrainSource.tests.cpp
    TerrainIdentity.tests.cpp
    TerrainLayout.tests.cpp
    TerrainPackFormat.tests.cpp
    TerrainPackIntegrity.tests.cpp
    TerrainRelease.tests.cpp
    TileAddressing.tests.cpp
    TileHeights.tests.cpp
)

# Committed release files that a later async reader must also read unchanged.
target_compile_definitions(engine_terrain_tests PRIVATE TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
