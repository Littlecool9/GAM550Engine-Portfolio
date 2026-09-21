engine_add_module(app)

target_link_libraries(engine_app
    PUBLIC mir::assets
           mir::core
           mir::window
           mir::renderer
           mir::editor
           mir::scene
           mir::script
    PRIVATE mir::input
            mir::animation
            mir::audio
            mir::physics
            mir::serial
)

target_sources(engine_app PRIVATE
    src/app/App.cpp
    src/app/AssetImportManager.cpp
    src/app/GameProject.cpp
    src/app/SceneResolve.cpp
    src/app/ModelSystem.cpp
)
