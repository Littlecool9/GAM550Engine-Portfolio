# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(editor)

find_package(Stb REQUIRED)
target_include_directories(engine_editor PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(engine_editor
    PUBLIC mir::imguiruntime
           mir::renderer
           mir::window
           mir::input
    PRIVATE mir::core
            mir::editortheme
            mir::serial
            imgui::imgui
            imguizmo::imguizmo
            iconfontcppheaders::iconfontcppheaders
)

# background image: open-file dialog and the LocalAppData lookup
if(WIN32)
    target_link_libraries(engine_editor PRIVATE ole32 shell32 uuid)
endif()

target_sources(engine_editor PRIVATE
    src/editor/BackgroundImage.cpp
    src/editor/Editor.cpp
    src/editor/EditorCamera.cpp
    src/editor/EditorPreferences.cpp
    src/editor/EntityBaseline.cpp
    src/editor/panels/HierarchyPanel.cpp
    src/editor/panels/AssetImportPanel.cpp
    src/editor/panels/InputSettingsPanel.cpp
    src/editor/panels/InspectorPanel.cpp
    src/editor/panels/LogPanel.cpp
    src/editor/panels/PreferencesPanel.cpp
    src/editor/panels/ProfilerPanel.cpp
    src/editor/panels/ProjectSettingsPanel.cpp
)
