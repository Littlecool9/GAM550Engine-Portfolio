engine_add_module_tests(gameui
    UIActiveDevice.tests.cpp
    UIDrawList.tests.cpp
    UIFocus.tests.cpp
    UIId.tests.cpp
    UILayout.tests.cpp
    UINavInput.tests.cpp
    UIState.tests.cpp
    UITextures.tests.cpp
    UIWidgets.tests.cpp
)

# UIWidgets.tests.cpp and UITextures.tests.cpp run a headless ImGui context, so
# EndFrame's submit and the UI's textures run
target_link_libraries(engine_gameui_tests PRIVATE imgui::imgui)
