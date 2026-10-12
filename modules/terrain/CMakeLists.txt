engine_add_module(terrain)

target_sources(engine_terrain PRIVATE
    src/terrain/ComponentRegistration.cpp
    src/terrain/TerrainAreaSamples.cpp
    src/terrain/TerrainCollision.cpp
    src/terrain/TerrainAssetPath.cpp
    src/terrain/TerrainByteSource.cpp
    src/terrain/TerrainExecutor.cpp
    src/terrain/TerrainHeightQuery.cpp
    src/terrain/TerrainLayout.cpp
    src/terrain/TerrainLod.cpp
    src/terrain/TerrainMeshResidency.cpp
    src/terrain/TerrainPackFormat.cpp
    src/terrain/TerrainPackWriter.cpp
    src/terrain/TerrainRegion.cpp
    src/terrain/TerrainRegionGrid.cpp
    src/terrain/TerrainRelease.cpp
    src/terrain/TerrainRenderMesh.cpp
    src/terrain/TerrainScene.cpp
    src/terrain/TerrainStreamer.cpp
    src/terrain/TerrainStreamingSettings.cpp
    src/terrain/TerrainSurface.cpp
    src/terrain/TerrainTileMesh.cpp
    src/terrain/TerrainViewSelection.cpp
    src/terrain/TileAddressing.cpp
    src/terrain/TileHeights.cpp
)

target_link_libraries(engine_terrain PUBLIC mir::assets mir::core PRIVATE mir::serial)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
