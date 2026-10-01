/**
 * @file Transform.tests.cpp
 * @author Sumin Park
 * @brief Tests for core/ecs/components/Transform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/components/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace
{
    using Catch::Approx;

    void RequireNear(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Approx(expected.x).margin(1e-5));
        CHECK(actual.y == Approx(expected.y).margin(1e-5));
        CHECK(actual.z == Approx(expected.z).margin(1e-5));
    }

    // The full local TRS in float; fine for these near-origin values.
    glm::mat4 LocalMatrix(const mir::Transform &t)
    {
        glm::mat4 m(t.LinearMatrix());
        m[3] = glm::vec4(glm::vec3(t.Position()), 1.0f);
        return m;
    }

    glm::vec3 Apply(const glm::mat4 &m, const glm::vec3 &p)
    {
        return glm::vec3(m * glm::vec4(p, 1.0f));
    }
}

TEST_CASE("Transform stays memcpy-safe for the ECS", "[ecs][transform]")
{
    // The column relocates rows with memcpy, so the traits are load-bearing,
    // not decoration. Restated here so a bad glm config fails a test rather
    // than only failing whichever translation unit happens to include it.
    STATIC_REQUIRE(std::is_trivially_copyable_v<mir::Transform>);
    STATIC_REQUIRE(std::is_standard_layout_v<mir::Transform>);
    STATIC_REQUIRE(alignof(mir::Transform) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

TEST_CASE("Default Transform is the identity", "[ecs][transform]")
{
    const mir::Transform t;
    RequireNear(Apply(LocalMatrix(t), glm::vec3(1.0f, 2.0f, 3.0f)), glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("Transform translates", "[ecs][transform]")
{
    mir::Transform t;
    t.SetPosition(glm::vec3(5.0f, -1.0f, 2.0f));

    RequireNear(Apply(LocalMatrix(t), glm::vec3(0.0f)), glm::vec3(5.0f, -1.0f, 2.0f));
}

TEST_CASE("Transform scales", "[ecs][transform]")
{
    mir::Transform t;
    t.SetScale(glm::vec3(2.0f, 3.0f, 4.0f));

    RequireNear(Apply(LocalMatrix(t), glm::vec3(1.0f)), glm::vec3(2.0f, 3.0f, 4.0f));
}

TEST_CASE("Transform rotates", "[ecs][transform]")
{
    mir::Transform t;
    // +90 degrees about Z takes +X to +Y
    t.SetRotation(glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));

    RequireNear(Apply(LocalMatrix(t), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("Transform applies scale, then rotation, then translation", "[ecs][transform]")
{
    // Order is the whole contract: scaling after rotating would shear, and
    // rotating the translation would move the pivot off the origin.
    mir::Transform t;
    t.SetPosition(glm::vec3(10.0f, 0.0f, 0.0f));
    t.SetRotation(glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
    t.SetScale(glm::vec3(2.0f));

    // (1,0,0) -> scale -> (2,0,0) -> rotate -> (0,2,0) -> translate -> (10,2,0)
    RequireNear(Apply(LocalMatrix(t), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(10.0f, 2.0f, 0.0f));

    // and it agrees with the naive three-matrix product it replaces
    const glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(t.Position())) *
                               glm::mat4_cast(t.Rotation()) *
                               glm::scale(glm::mat4(1.0f), t.Scale());
    for (int c = 0; c < 4; ++c)
        RequireNear(glm::vec3(LocalMatrix(t)[c]), glm::vec3(expected[c]));
}

TEST_CASE("Transform keeps a millimeter step at the map edge", "[ecs][transform][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // A float position spaced 1/64 m at 150 km would round the step away.
    mir::Transform t{mir::WorldVec3(-150000.125, 250.0, 75000.5)};
    REQUIRE(t.Translate(mir::WorldVec3(0.001, -0.002, 0.003)));

    CHECK(t.Position().x == Approx(-150000.124).margin(1e-9));
    CHECK(t.Position().y == Approx(249.998).margin(1e-9));
    CHECK(t.Position().z == Approx(75000.503).margin(1e-9));
    CHECK(static_cast<float>(t.Position().x) == static_cast<float>(-150000.125));
}

TEST_CASE("Transform refuses a non-finite position without a version bump", "[ecs][transform][lwc]")
{
    constexpr mir::WorldReal kMax = std::numeric_limits<mir::WorldReal>::max();
    mir::Transform t{mir::WorldVec3(1.0, 2.0, 3.0)};
    const uint32_t version = t.Version();

    CHECK_FALSE(t.SetPosition(mir::WorldVec3(std::numeric_limits<mir::WorldReal>::quiet_NaN(), 0.0, 0.0)));
    CHECK_FALSE(t.SetPosition(mir::WorldVec3(0.0, std::numeric_limits<mir::WorldReal>::infinity(), 0.0)));
    CHECK(t.Version() == version);
    CHECK(t.Position() == mir::WorldVec3(1.0, 2.0, 3.0));

    // an overflowing sum is refused too
    REQUIRE(t.SetPosition(mir::WorldVec3(kMax, 0.0, 0.0)));
    const uint32_t atMax = t.Version();
    CHECK_FALSE(t.Translate(mir::WorldVec3(kMax, 0.0, 0.0)));
    CHECK(t.Version() == atMax);
    CHECK(t.Position().x == kMax);
}
