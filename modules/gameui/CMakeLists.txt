engine_add_module(gameui)

# PUBLIC: AssetId (assets) and InputState (input) appear in include/gameui/.
# imgui is PRIVATE and reaches only UIBackendImGui.cpp; no ImGui type may
# appear in a public header. The renderer is not needed: the ImGui backend
# uploads the UI's textures itself.
target_link_libraries(engine_gameui
    PUBLIC mir::assets
           mir::input
           mir::core
    PRIVATE imgui::imgui
)

target_sources(engine_gameui PRIVATE
    src/gameui/UIBackendImGui.cpp
    src/gameui/UIContext.cpp
    src/gameui/UIDemoScreen.cpp
    src/gameui/UIDrawList.cpp
    src/gameui/UINavInput.cpp
    src/gameui/UIState.cpp
    src/gameui/UITextures.cpp
    src/gameui/UIWidgets.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
