engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
)

target_link_libraries(engine_script_tests PRIVATE mir::input)
