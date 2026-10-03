engine_add_module_tests(gameshell
    GameShell.tests.cpp
)

# runs a headless ImGui context, so the screens draw and submit as in App
target_link_libraries(engine_gameshell_tests PRIVATE imgui::imgui)
