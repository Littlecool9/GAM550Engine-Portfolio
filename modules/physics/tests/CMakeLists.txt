engine_add_module_tests(physics
    CollisionEvents.tests.cpp
    HeightField.tests.cpp
    PhysicsSystem.tests.cpp
    PhysicsWorld.tests.cpp
    TerrainCollider.tests.cpp
    TerrainCollision.tests.cpp
    TerrainCollisionResidency.tests.cpp
)

# Test-only: checks contact against the terrain surface; engine_physics never links terrain.
target_link_libraries(engine_physics_tests PRIVATE mir::terrain terrain_bake)
target_compile_definitions(engine_physics_tests PRIVATE
    TERRAIN_COLLISION_RECIPE="${PROJECT_SOURCE_DIR}/tools/TerrainTool/tests/fixtures/mixed-2x2.terrain.json")
