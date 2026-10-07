engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    PhysicsBindings.tests.cpp
    AudioBindings.tests.cpp
    TerrainBindings.tests.cpp
)


target_link_libraries(engine_script_tests PRIVATE mir::audio mir::input mir::physics mir::terrain sol2::sol2 PkgConfig::LuaJIT)
