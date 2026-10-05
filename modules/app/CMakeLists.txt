engine_add_module(app)

target_link_libraries(engine_app
    PUBLIC mir::assets
           mir::core
           mir::window
           mir::renderer
           mir::gameui
           mir::gameshell
           mir::imguiruntime
           mir::scene
           mir::script
    PRIVATE mir::input
            mir::animation
            mir::audio
            mir::physics
            mir::serial
            mir::terrain
)

target_sources(engine_app PRIVATE
    src/app/App.cpp
    src/app/GameProject.cpp
    src/app/SceneResolve.cpp
    src/app/TerrainRender.cpp
)

# PUBLIC: App.h holds the editor's members only with it, so everything that
# includes App.h has to agree on which App it is
target_compile_definitions(engine_app PUBLIC MIR_WITH_EDITOR=$<BOOL:${MJOLNIR_WITH_EDITOR}>)

# the editor, and the asset import queue that only its panel drives
if(MJOLNIR_WITH_EDITOR)
    target_link_libraries(engine_app PUBLIC mir::editor)
    target_sources(engine_app PRIVATE src/app/AssetImportManager.cpp)
endif()
