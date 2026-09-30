/**
 * @file WorldCoordinates.tests.cpp
 * @brief Fixed-world coordinate arithmetic and explicit narrowing tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */

#include <core/math/WorldCoordinates.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <type_traits>

namespace
{
    double DoubleErrorBudget(double referenceMeters)
    {
        return 4.0 * std::numeric_limits<double>::epsilon() *
               std::max(1.0, std::abs(referenceMeters));
    }

    void CheckNear(const glm::dvec3 &actual, const glm::dvec3 &expected, double budget)
    {
        CHECK(std::abs(actual.x - expected.x) <= budget);
        CHECK(std::abs(actual.y - expected.y) <= budget);
        CHECK(std::abs(actual.z - expected.z) <= budget);
    }
}

TEST_CASE("Fixed-world coordinates remain ECS-compatible value types", "[core][lwc]")
{
    STATIC_REQUIRE(std::is_trivially_copyable_v<mir::WorldPosition>);
    STATIC_REQUIRE(std::is_standard_layout_v<mir::WorldPosition>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mir::WorldOffset>);
    STATIC_REQUIRE(std::is_standard_layout_v<mir::WorldOffset>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mir::ParentLocalTranslation>);
    STATIC_REQUIRE(std::is_standard_layout_v<mir::ParentLocalTranslation>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mir::TerrainLocalPosition>);
    STATIC_REQUIRE(std::is_standard_layout_v<mir::TerrainLocalPosition>);
    STATIC_REQUIRE(alignof(mir::WorldPosition) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);

    const mir::WorldPosition origin;
    CHECK(origin.Meters() == glm::dvec3(0.0));
}

TEST_CASE("Near-origin world subtraction and renderer conversion", "[core][lwc]")
{
    const auto camera = mir::WorldPosition::FromMeters({-2.5, 4.0, 3.75});
    const auto offset = mir::WorldOffset::FromMeters({0.125, -0.0625, 0.25});
    REQUIRE(camera);
    REQUIRE(offset);

    const auto point = mir::TryTranslateWorld(*camera, *offset);
    REQUIRE(point);
    const auto recovered = mir::TryWorldDifference(*point, *camera);
    REQUIRE(recovered);
    CHECK(recovered->Meters() == offset->Meters());

    const auto rendererLocal = mir::TryCameraRelativeFloat(*point, *camera);
    REQUIRE(rendererLocal);
    CHECK(*rendererLocal == glm::vec3(0.125f, -0.0625f, 0.25f));
}

TEST_CASE("Map-edge small displacements survive before float narrowing", "[core][lwc]")
{
    struct Case
    {
        glm::dvec3 camera;
        glm::dvec3 displacement;
    };
    const Case cases[] = {
        {{75000.25, 125.0, -75000.5}, {0.001, -0.002, 0.003}},
        {{-150000.125, -20.0, 150000.625}, {-0.001, 0.002, -0.003}},
    };

    for (const Case &test : cases)
    {
        const auto camera = mir::WorldPosition::FromMeters(test.camera);
        const auto displacement = mir::WorldOffset::FromMeters(test.displacement);
        REQUIRE(camera);
        REQUIRE(displacement);

        const auto point = mir::TryTranslateWorld(*camera, *displacement);
        REQUIRE(point);
        const auto recovered = mir::TryWorldDifference(*point, *camera);
        REQUIRE(recovered);

        const double doubleBudget = DoubleErrorBudget(
            std::max(std::abs(test.camera.x), std::abs(test.camera.z)));
        CheckNear(recovered->Meters(), test.displacement, doubleBudget);

        const auto rendererLocal = mir::TryCameraRelativeFloat(*point, *camera);
        REQUIRE(rendererLocal);
        const double floatBudget = doubleBudget +
            std::numeric_limits<float>::epsilon() *
                std::max({std::abs(test.displacement.x),
                          std::abs(test.displacement.y),
                          std::abs(test.displacement.z)});
        CheckNear(glm::dvec3(*rendererLocal), test.displacement, floatBudget);

        const glm::vec3 prematurelyNarrowed =
            glm::vec3(point->Meters()) - glm::vec3(camera->Meters());
        CHECK(prematurelyNarrowed.x == 0.0f);

        const glm::dvec3 error = glm::abs(recovered->Meters() - test.displacement);
        const glm::dvec3 rendererError = glm::abs(glm::dvec3(*rendererLocal) - test.displacement);
        std::cout << std::setprecision(17)
                  << "LWC camera=(" << test.camera.x << ", " << test.camera.y << ", " << test.camera.z
                  << ") displacement=(" << test.displacement.x << ", " << test.displacement.y << ", " << test.displacement.z
                  << ") double_abs_error_m=(" << error.x << ", " << error.y << ", " << error.z
                  << ") renderer_abs_error_m=(" << rendererError.x << ", " << rendererError.y << ", " << rendererError.z
                  << ")\n";
    }
}

TEST_CASE("Camera-relative result belongs to its viewport camera", "[core][lwc]")
{
    const auto point = mir::WorldPosition::FromMeters({150000.125, 12.0, -75000.5});
    const auto firstCamera = mir::WorldPosition::FromMeters({150000.0, 12.0, -75000.5});
    const auto secondCamera = mir::WorldPosition::FromMeters({149990.0, 12.0, -75000.5});
    REQUIRE(point);
    REQUIRE(firstCamera);
    REQUIRE(secondCamera);

    const auto first = mir::TryCameraRelativeFloat(*point, *firstCamera);
    const auto second = mir::TryCameraRelativeFloat(*point, *secondCamera);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->x == 0.125f);
    CHECK(second->x == 10.125f);
}

TEST_CASE("Parent-local translation becomes world-axis displacement after linear transform", "[core][lwc]")
{
    const auto local = mir::ParentLocalTranslation::FromMeters({0.001, 2.0, 3.0});
    REQUIRE(local);

    glm::dmat3 parentLinear(1.0);
    parentLinear[0] = {0.0, 2.0, 0.0};
    parentLinear[1] = {-3.0, 0.0, 0.0};
    const auto worldOffset = mir::TryParentLocalToWorldOffset(parentLinear, *local);
    REQUIRE(worldOffset);
    CheckNear(worldOffset->Meters(), {-6.0, 0.002, 3.0}, 1e-15);

    const auto parent = mir::WorldPosition::FromMeters({150000.0, 500.0, -150000.0});
    REQUIRE(parent);
    const auto child = mir::TryTranslateWorld(*parent, *worldOffset);
    REQUIRE(child);
    const auto recovered = mir::TryWorldDifference(*child, *parent);
    REQUIRE(recovered);
    CheckNear(recovered->Meters(), worldOffset->Meters(), DoubleErrorBudget(150000.0));
}

TEST_CASE("Translated terrain instance round-trips local coordinates", "[core][lwc]")
{
    const auto instance = mir::WorldPosition::FromMeters({123456.125, 350.5, -76543.75});
    const auto local = mir::TerrainLocalPosition::FromMeters({-256.001, 12.375, 512.002});
    REQUIRE(instance);
    REQUIRE(local);

    const auto world = mir::TryTerrainLocalToWorld(*local, *instance);
    REQUIRE(world);
    const auto recovered = mir::TryWorldToTerrainLocal(*world, *instance);
    REQUIRE(recovered);
    CheckNear(recovered->Meters(), local->Meters(), DoubleErrorBudget(123456.125));

    const glm::dvec3 error = glm::abs(recovered->Meters() - local->Meters());
    std::cout << std::setprecision(17)
              << "LWC terrain_instance=(" << instance->Meters().x << ", " << instance->Meters().y << ", " << instance->Meters().z
              << ") local=(" << local->Meters().x << ", " << local->Meters().y << ", " << local->Meters().z
              << ") roundtrip_abs_error_m=(" << error.x << ", " << error.y << ", " << error.z << ")\n";
}

TEST_CASE("Invalid coordinates and overflowing conversions fail explicitly", "[core][lwc]")
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double maxDouble = std::numeric_limits<double>::max();
    REQUIRE_FALSE(mir::WorldPosition::FromMeters({nan, 0.0, 0.0}));
    REQUIRE_FALSE(mir::WorldOffset::FromMeters({infinity, 0.0, 0.0}));
    REQUIRE_FALSE(mir::ParentLocalTranslation::FromMeters({0.0, nan, 0.0}));
    REQUIRE_FALSE(mir::TerrainLocalPosition::FromMeters({0.0, 0.0, -infinity}));

    const auto large = mir::WorldPosition::FromMeters({maxDouble, 0.0, 0.0});
    const auto negative = mir::WorldPosition::FromMeters({-maxDouble, 0.0, 0.0});
    const auto largeOffset = mir::WorldOffset::FromMeters({maxDouble, 0.0, 0.0});
    const auto largeLocal = mir::TerrainLocalPosition::FromMeters({maxDouble, 0.0, 0.0});
    REQUIRE(large);
    REQUIRE(negative);
    REQUIRE(largeOffset);
    REQUIRE(largeLocal);
    CHECK_FALSE(mir::TryTranslateWorld(*large, *largeOffset));
    CHECK_FALSE(mir::TryWorldDifference(*large, *negative));
    CHECK_FALSE(mir::TryCameraRelativeFloat(*large, mir::WorldPosition{}));
    CHECK_FALSE(mir::TryRendererLocalFloat(*largeOffset));
    CHECK_FALSE(mir::TryTerrainLocalToWorld(*largeLocal, *large));
    CHECK_FALSE(mir::TryWorldToTerrainLocal(*large, *negative));

    glm::dmat3 invalidLinear(1.0);
    invalidLinear[0][0] = nan;
    const auto local = mir::ParentLocalTranslation::FromMeters({1.0, 0.0, 0.0});
    REQUIRE(local);
    CHECK_FALSE(mir::TryParentLocalToWorldOffset(invalidLinear, *local));

    glm::dmat3 overflowingLinear(1.0);
    overflowingLinear[0][0] = maxDouble;
    const auto twice = mir::ParentLocalTranslation::FromMeters({2.0, 0.0, 0.0});
    REQUIRE(twice);
    CHECK_FALSE(mir::TryParentLocalToWorldOffset(overflowingLinear, *twice));

    const auto bounded = mir::WorldOffset::FromMeters({0.1, std::numeric_limits<double>::denorm_min(), 0.0});
    REQUIRE(bounded);
    const auto narrowed = mir::TryRendererLocalFloat(*bounded);
    REQUIRE(narrowed);
    CHECK(static_cast<double>(narrowed->x) != 0.1);
    CHECK(narrowed->y == 0.0f);
}
