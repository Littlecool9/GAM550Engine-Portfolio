engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    ScriptReloadWatcher.tests.cpp
)

# ScriptReloadWatcher.h names AssetId, and script links assets privately
target_link_libraries(engine_script_tests PRIVATE mir::assets mir::input)
