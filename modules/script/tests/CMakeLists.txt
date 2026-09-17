engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    PhysicsBindings.tests.cpp
)

# engine_script links both PRIVATE, so the tests have to ask for their include
# directories themselves.
target_link_libraries(engine_script_tests PRIVATE mir::input mir::physics)
