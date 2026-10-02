engine_add_module(terrain)

target_sources(engine_terrain PRIVATE
    src/terrain/TerrainAssetPath.cpp
    src/terrain/TerrainByteSource.cpp
    src/terrain/TerrainHeightQuery.cpp
    src/terrain/TerrainLayout.cpp
    src/terrain/TerrainPackFormat.cpp
    src/terrain/TerrainPackWriter.cpp
    src/terrain/TerrainRelease.cpp
    src/terrain/TerrainSurface.cpp
    src/terrain/TerrainTileMesh.cpp
    src/terrain/TileAddressing.cpp
    src/terrain/TileHeights.cpp
)

target_link_libraries(engine_terrain PUBLIC mir::assets mir::core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
