engine_add_module(imguiruntime)

target_link_libraries(engine_imguiruntime
    PUBLIC mir::renderer
           mir::window
    PRIVATE mir::core
            imgui::imgui
            iconfontcppheaders::iconfontcppheaders
)

target_sources(engine_imguiruntime PRIVATE src/imguiruntime/ImGuiRuntime.cpp)
