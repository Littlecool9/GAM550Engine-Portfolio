/**
 * @file CameraMath.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/CameraMath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/CameraMath.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>

namespace
{
    using mir::WorldPose;
    using mir::WorldPosition;
    using mir::WorldRay;

    // Map-edge coordinates shared with terrain ADRs 0002 to 0004.
    const glm::dvec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const glm::dvec3 kEdgeB{150000.625, -20.0, -75000.25};

    const mir::Camera kLens{60.0f, 0.1f, 100.0f};
    const glm::vec2 kCenter{0.0f, 0.0f};

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromMeters(meters);
        REQUIRE(position);
        return *position;
    }

    // An identity camera looks down -Z.
    WorldRay CenterRayFrom(const glm::dvec3 &meters)
    {
        return mir::MakeCameraRay(WorldPose{At(meters)}, kLens, 1.0f, kCenter);
    }

    std::optional<float> HitThinBox(const WorldRay &ray, const glm::dvec3 &center)
    {
        return mir::RayIntersectsBox(ray, WorldPose{At(center)}, glm::vec3(-0.01f), glm::vec3(0.01f));
    }
}

TEST_CASE("The projection maps near to far onto Vulkan's 0 to 1 depth", "[renderer]")
{
    const glm::mat4 viewProj =
        mir::MakeViewProjection(glm::mat4(1.0f), kLens.mFovYDegrees, kLens.mNearZ, kLens.mFarZ, 1.0f);

    const glm::vec4 nearClip = viewProj * glm::vec4(0.0f, 0.0f, -kLens.mNearZ, 1.0f);
    const glm::vec4 farClip = viewProj * glm::vec4(0.0f, 0.0f, -kLens.mFarZ, 1.0f);
    CHECK(std::abs(nearClip.z / nearClip.w) <= 1e-6f);
    CHECK(std::abs(farClip.z / farClip.w - 1.0f) <= 1e-6f);

    // +y up in the world is -y in Vulkan NDC
    const glm::vec4 above = viewProj * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
    CHECK(above.y / above.w < 0.0f);
}

TEST_CASE("A camera ray starts at the camera's double position", "[renderer][lwc]")
{
    const WorldRay ray = CenterRayFrom(kEdgeA);

    CHECK(ray.origin.Meters() == kEdgeA);
    CHECK(std::abs(glm::length(ray.direction) - 1.0f) <= 1e-6f);
    CHECK(std::abs(ray.direction.x) <= 1e-6f);
    CHECK(std::abs(ray.direction.y) <= 1e-6f);
    CHECK(ray.direction.z < -0.999f);

    // only the origin depends on where the camera is
    CHECK(CenterRayFrom(kEdgeB).direction == ray.direction);
    CHECK(CenterRayFrom(glm::dvec3(0.0)).direction == ray.direction);
}

TEST_CASE("A camera ray follows the viewport's NDC", "[renderer]")
{
    const WorldPose camera{At(kEdgeB)};

    // Vulkan NDC: +x right, +y down
    const WorldRay right = mir::MakeCameraRay(camera, kLens, 1.0f, glm::vec2(1.0f, 0.0f));
    const WorldRay top = mir::MakeCameraRay(camera, kLens, 1.0f, glm::vec2(0.0f, -1.0f));
    CHECK(right.direction.x > 0.0f);
    CHECK(top.direction.y > 0.0f);

    // the edge of a 60 degree vertical field is 30 degrees off the axis
    CHECK(std::abs(top.direction.y - std::sin(glm::radians(30.0f))) <= 1e-5f);

    const glm::mat3 turn = glm::mat3_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const WorldRay turned = mir::MakeCameraRay(WorldPose{At(kEdgeB), turn}, kLens, 1.0f, kCenter);
    CHECK(turned.direction.x < -0.999f);
}

TEST_CASE("A far ray tells boxes two millimeters apart", "[renderer][lwc]")
{
    const WorldRay ray = CenterRayFrom(kEdgeA);
    const glm::dvec3 ahead = kEdgeA + glm::dvec3(0.0, 0.0, -1.7);

    // a 2 cm box: 9 mm off the axis it still covers the ray, 11 mm off it does not
    const std::optional<float> hit = HitThinBox(ray, ahead + glm::dvec3(0.009, 0.0, 0.0));
    REQUIRE(hit);
    CHECK(std::abs(*hit - 1.69f) <= 1e-5f);
    CHECK_FALSE(HitThinBox(ray, ahead + glm::dvec3(0.011, 0.0, 0.0)));

    // as float world positions the two boxes are the same point
    CHECK(static_cast<float>(ahead.x + 0.009) == static_cast<float>(ahead.x + 0.011));
}

TEST_CASE("A far pick matches the same pick at the origin", "[renderer][lwc]")
{
    const glm::mat3 turn = glm::mat3_cast(glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::mat3 boxLinear =
        glm::mat3_cast(glm::angleAxis(glm::radians(40.0f), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)))) *
        glm::mat3(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.5f));
    const glm::dvec3 boxOffset = glm::dvec3(turn * glm::vec3(0.2f, -0.1f, -6.0f));
    const glm::vec2 ndc{0.05f, 0.02f};

    std::optional<float> hits[3];
    const glm::dvec3 cameras[3] = {glm::dvec3(0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        const WorldRay ray = mir::MakeCameraRay(WorldPose{At(cameras[i]), turn}, kLens, 1.5f, ndc);
        hits[i] = mir::RayIntersectsBox(ray, WorldPose{At(cameras[i] + boxOffset), boxLinear},
                                        glm::vec3(-0.5f), glm::vec3(0.5f));
        REQUIRE(hits[i]);
    }

    CHECK(*hits[0] > 4.0f);
    CHECK(*hits[0] < 6.5f);
    CHECK(std::abs(*hits[1] - *hits[0]) <= 1e-4f);
    CHECK(std::abs(*hits[2] - *hits[0]) <= 1e-4f);
}

TEST_CASE("Each camera's ray picks only what is in front of it", "[renderer][lwc]")
{
    const glm::dvec3 boxNearA = kEdgeA + glm::dvec3(0.0, 0.0, -3.0);
    const WorldRay fromA = CenterRayFrom(kEdgeA);
    const WorldRay fromB = CenterRayFrom(kEdgeB);

    CHECK(HitThinBox(fromA, boxNearA));
    CHECK_FALSE(HitThinBox(fromB, boxNearA));

    // a second camera beside the first sees the same box from its own origin
    const WorldRay beside = CenterRayFrom(kEdgeA + glm::dvec3(0.0, 0.0, 2.0));
    const std::optional<float> nearHit = HitThinBox(fromA, boxNearA);
    const std::optional<float> farHit = HitThinBox(beside, boxNearA);
    REQUIRE(nearHit);
    REQUIRE(farHit);
    CHECK(std::abs(*farHit - *nearHit - 2.0f) <= 1e-5f);
}

TEST_CASE("The hit distance is meters along the ray for a scaled box", "[renderer]")
{
    const WorldRay ray = CenterRayFrom(kEdgeB);
    const WorldPose box{At(kEdgeB + glm::dvec3(0.0, 0.0, -10.0)), glm::mat3(3.0f)};

    const std::optional<float> hit = mir::RayIntersectsBox(ray, box, glm::vec3(-0.5f), glm::vec3(0.5f));
    REQUIRE(hit);
    CHECK(std::abs(*hit - 8.5f) <= 1e-4f);

    // behind the camera is not a hit
    const WorldPose behind{At(kEdgeB + glm::dvec3(0.0, 0.0, 10.0)), glm::mat3(3.0f)};
    CHECK_FALSE(mir::RayIntersectsBox(ray, behind, glm::vec3(-0.5f), glm::vec3(0.5f)));
}

TEST_CASE("A box beyond float range of the ray is not hit", "[renderer][lwc]")
{
    const WorldRay ray = CenterRayFrom(glm::dvec3(0.0));
    const WorldPose unreachable{At(glm::dvec3(0.0, 0.0, -1e300))};

    CHECK_FALSE(mir::RayIntersectsBox(ray, unreachable, glm::vec3(-0.5f), glm::vec3(0.5f)));
}
