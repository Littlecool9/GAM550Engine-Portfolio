# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(editor)

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

target_sources(engine_editor PRIVATE
    src/editor/Editor.cpp
    src/editor/EditorCamera.cpp
    src/editor/EntityBaseline.cpp
    src/editor/IdeSettings.cpp
    src/editor/panels/HierarchyPanel.cpp
    src/editor/panels/AssetImportPanel.cpp
    src/editor/panels/ContentBrowserPanel.cpp
    src/editor/panels/InputSettingsPanel.cpp
    src/editor/panels/InspectorPanel.cpp
    src/editor/panels/LogPanel.cpp
    src/editor/panels/ProfilerPanel.cpp
    src/editor/panels/ProjectSettingsPanel.cpp
)
