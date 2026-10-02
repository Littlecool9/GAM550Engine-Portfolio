engine_add_module(terrain)

target_sources(engine_terrain PRIVATE
    src/terrain/MockTerrainSource.cpp
    src/terrain/TerrainByteSource.cpp
    src/terrain/TerrainLayout.cpp
    src/terrain/TerrainPackFormat.cpp
    src/terrain/TerrainPackWriter.cpp
    src/terrain/TerrainRelease.cpp
    src/terrain/TileAddressing.cpp
    src/terrain/TileHeights.cpp
)

# No renderer or physics: this is terrain data, shared by the engine and a future Terrain Tool.
# Only the release reader reads files, through core FileIO.
target_link_libraries(engine_terrain PUBLIC mir::assets mir::core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
