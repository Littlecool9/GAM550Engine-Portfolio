# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(editortheme)

target_link_libraries(engine_editortheme PRIVATE imgui::imgui)

target_sources(engine_editortheme PRIVATE src/editortheme/EditorTheme.cpp)
