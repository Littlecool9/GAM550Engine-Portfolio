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

#include <algorithm>
#include <cmath>
#include <limits>
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
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
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

namespace
{
    const mir::Camera kNoFarLens{60.0f, 0.1f, 0.0f};
    const mir::Camera kTerrainLens{60.0f, 0.1f, 120000.0f};

    float DepthAt(const mir::Camera &lens, float distance)
    {
        const glm::vec4 clip = mir::MakeViewProjection(glm::mat4(1.0f), lens, 1.0f) * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
        return clip.z / clip.w;
    }
}

TEST_CASE("Directional shadows cover the rendered frustum beyond the old camera box", "[renderer][shadow]")
{
    const glm::mat3 orientation = glm::mat3_cast(glm::angleAxis(0.7f, glm::vec3(0, 1, 0)));
    const WorldPose camera{At(kEdgeB), orientation};
    for (const mir::Camera lens : {kLens, kNoFarLens, kTerrainLens})
        for (const glm::vec3 sun : {glm::vec3(0, 1, 0), glm::vec3(-0.3f, 0.8f, -0.2f)})
        {
            const auto projection = mir::MakeDirectionalShadowViewProjection(camera, lens, 1.8f, sun, 512);
            const float end = lens.HasFarPlane() ? std::min(lens.mFarZ, 512.0f) : 512.0f;
            for (float distance : {lens.mNearZ, end})
                for (float x : {-1.f, 1.f}) for (float y : {-1.f, 1.f})
                {
                    const float halfHeight = distance * std::tan(glm::radians(lens.mFovYDegrees) / 2);
                    const auto relative = orientation * glm::vec3(x * halfHeight * 1.8f, y * halfHeight, -distance);
                    const auto clip = projection * glm::vec4(relative, 1);
                    CHECK(std::abs(clip.x) < clip.w);
                    CHECK(std::abs(clip.y) < clip.w);
                    CHECK(clip.z >= 0);
                    CHECK(clip.z <= clip.w);
                }
            const WorldPose originCamera{At(glm::dvec3(0)), orientation};
            CHECK(projection == mir::MakeDirectionalShadowViewProjection(originCamera, lens, 1.8f, sun, 512));
        }
}

TEST_CASE("The projection is reverse-Z: near maps to depth 1 and far to 0", "[renderer]")
{
    CHECK(std::abs(DepthAt(kLens, kLens.mNearZ) - 1.0f) <= 1e-6f);
    CHECK(std::abs(DepthAt(kLens, kLens.mFarZ)) <= 1e-6f);
    CHECK(DepthAt(kLens, 1.0f) > DepthAt(kLens, 2.0f));

    const glm::mat4 viewProj = mir::MakeViewProjection(glm::mat4(1.0f), kLens, 2.0f);

    // clip w is the view depth, which the transparent sort relies on
    const glm::vec4 ahead = viewProj * glm::vec4(0.0f, 0.0f, -5.0f, 1.0f);
    CHECK(ahead.w == 5.0f);

    // +y up in the world is -y in Vulkan NDC; the top of a 60 degree field is -1
    const float top = 5.0f * std::tan(glm::radians(30.0f));
    const glm::vec4 above = viewProj * glm::vec4(0.0f, top, -5.0f, 1.0f);
    CHECK(std::abs(above.y / above.w + 1.0f) <= 1e-6f);

    // aspect 2 puts the same offset halfway to the right edge
    const glm::vec4 right = viewProj * glm::vec4(top, 0.0f, -5.0f, 1.0f);
    CHECK(std::abs(right.x / right.w - 0.5f) <= 1e-6f);
}

TEST_CASE("A lens with no far plane never clips by distance", "[renderer]")
{
    CHECK(kLens.HasFarPlane());
    CHECK_FALSE(kNoFarLens.HasFarPlane());
    // anything that is not a plane beyond the near one also means "none"
    CHECK_FALSE(mir::Camera{60.0f, 0.1f, -5.0f}.HasFarPlane());
    CHECK_FALSE(mir::Camera{60.0f, 0.1f, 0.1f}.HasFarPlane());
    CHECK_FALSE(mir::Camera{60.0f, 0.1f, std::numeric_limits<float>::infinity()}.HasFarPlane());
    CHECK_FALSE(mir::Camera{60.0f, 0.1f, std::nanf("")}.HasFarPlane());

    CHECK(std::abs(DepthAt(kNoFarLens, kNoFarLens.mNearZ) - 1.0f) <= 1e-6f);

    // depth falls toward 0 and never reaches the clip boundary
    float previous = 1.0f;
    for (const float distance : {1.0f, 100.0f, 1e4f, 1e6f, 1e9f})
    {
        const float depth = DepthAt(kNoFarLens, distance);
        CHECK(depth > 0.0f);
        CHECK(depth < previous);
        previous = depth;
    }

    // a degenerate lens still projects instead of dividing by zero
    const float degenerate = DepthAt(mir::Camera{60.0f, 0.1f, 0.1f}, 50.0f);
    CHECK(std::isfinite(degenerate));
    CHECK(degenerate == DepthAt(kNoFarLens, 50.0f));
}

TEST_CASE("Reverse-Z depth tells far surfaces apart where the usual mapping cannot", "[renderer]")
{
    for (const mir::Camera &lens : {kTerrainLens, kNoFarLens})
    {
        for (const float distance : {100.0f, 1e3f, 1e4f, 5e4f})
        {
            // a gap of one part in ten thousand: 1 m at 10 km
            CHECK(DepthAt(lens, distance) > DepthAt(lens, distance * 1.0001f));
        }
    }

    // the near-to-0, far-to-1 mapping this replaced, in the same float
    const auto conventional = [](float distance)
    {
        const float n = kTerrainLens.mNearZ;
        const float f = kTerrainLens.mFarZ;
        return f / (f - n) * (1.0f - n / distance);
    };
    CHECK(conventional(1e4f) == conventional(1e4f * 1.0001f));
}

TEST_CASE("A camera ray starts at the camera's double position", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const WorldRay ray = CenterRayFrom(kEdgeA);

    CHECK(glm::dvec3(ray.origin.Meters()) == kEdgeA);
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
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

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
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

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
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const WorldRay ray = CenterRayFrom(glm::dvec3(0.0));
    const WorldPose unreachable{At(glm::dvec3(0.0, 0.0, -1e300))};

    CHECK_FALSE(mir::RayIntersectsBox(ray, unreachable, glm::vec3(-0.5f), glm::vec3(0.5f)));
}

TEST_CASE("A camera ray does not depend on the lens's clip planes", "[renderer]")
{
    const WorldPose camera{At(kEdgeB)};
    const float aspect = 16.0f / 9.0f;
    const double tanHalf = std::tan(glm::radians(30.0));

    for (const glm::vec2 &ndc : {glm::vec2(0.0f, 0.0f), glm::vec2(0.5f, -0.3f), glm::vec2(-1.0f, 1.0f), glm::vec2(0.99f, 0.99f)})
    {
        const glm::dvec3 expected = glm::normalize(glm::dvec3(ndc.x * tanHalf * aspect, -ndc.y * tanHalf, -1.0));

        for (const mir::Camera &lens : {kLens, kTerrainLens, kNoFarLens, mir::Camera{60.0f, 0.01f, 0.0f}})
        {
            const WorldRay ray = mir::MakeCameraRay(camera, lens, aspect, ndc);
            // the sine of the angle between the two directions
            CHECK(glm::length(glm::cross(glm::dvec3(ray.direction), expected)) <= 1e-6);
        }
    }
}
