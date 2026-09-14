engine_add_module(app)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(engine_app
    PUBLIC mir::assets
           mir::core
           mir::window
           mir::renderer
           mir::editor
           mir::scene
           mir::script
    PRIVATE mir::input
            nlohmann_json::nlohmann_json
)

target_sources(engine_app PRIVATE
    src/app/App.cpp
    src/app/GameProject.cpp
    src/app/SceneResolve.cpp
)
