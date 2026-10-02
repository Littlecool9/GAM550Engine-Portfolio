engine_add_module(assets)

target_sources(engine_assets PRIVATE
    src/assets/AssetBlob.cpp
    src/assets/AssetCache.cpp
    src/assets/AssetId.cpp
    src/assets/AssetManifest.cpp
    src/assets/AssetPath.cpp
    src/assets/ModelAsset.cpp
)

target_link_libraries(engine_assets PUBLIC mir::core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
