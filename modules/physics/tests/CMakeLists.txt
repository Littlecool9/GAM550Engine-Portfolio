engine_add_module_tests(physics
    CollisionEvents.tests.cpp
    HeightField.tests.cpp
    PhysicsSystem.tests.cpp
    PhysicsWorld.tests.cpp
    TerrainCollider.tests.cpp
)

# Test-only: checks contact against the terrain surface; engine_physics never links terrain.
target_link_libraries(engine_physics_tests PRIVATE mir::terrain)
