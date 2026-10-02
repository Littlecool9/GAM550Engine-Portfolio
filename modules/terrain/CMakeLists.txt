engine_add_module(terrain)

target_sources(engine_terrain PRIVATE
    src/terrain/MockTerrainSource.cpp
    src/terrain/TerrainLayout.cpp
    src/terrain/TileAddressing.cpp
)

# No renderer or physics: this is terrain data, shared by the engine and a future Terrain Tool.
target_link_libraries(engine_terrain PUBLIC mir::assets mir::core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
