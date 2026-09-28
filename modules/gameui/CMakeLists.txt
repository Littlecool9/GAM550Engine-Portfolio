engine_add_module(gameui)

# PUBLIC: TextureHandle (renderer) and InputState (input) appear in
# include/gameui/. imgui is PRIVATE and reaches only UIBackendImGui.cpp; no
# ImGui type may appear in a public header.
target_link_libraries(engine_gameui
    PUBLIC mir::renderer
           mir::input
           mir::core
    PRIVATE imgui::imgui
)

target_sources(engine_gameui PRIVATE
    src/gameui/UIBackendImGui.cpp
    src/gameui/UIContext.cpp
    src/gameui/UIDrawList.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
