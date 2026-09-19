engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    PhysicsBindings.tests.cpp
    AudioBindings.tests.cpp
)

# mir::audio drags Wwise in at link time. The SDK is already REQUIRED for the
# whole build, so this adds no new prerequisite - only a bigger test binary.
target_link_libraries(engine_script_tests PRIVATE mir::audio mir::input mir::physics)
