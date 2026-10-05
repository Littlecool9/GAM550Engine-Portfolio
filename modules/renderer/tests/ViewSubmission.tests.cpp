/**
 * @file ViewSubmission.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/ViewSubmission.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ViewSubmission.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace
{
    using mir::DrawItem;
    using mir::WorldDrawItem;
    using mir::WorldPose;
    using mir::WorldPosition;

    // Map-edge coordinates shared with terrain ADRs 0002 and 0003.
    const glm::dvec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const glm::dvec3 kEdgeB{150000.625, -20.0, -75000.25};
    const glm::dvec3 kStep{0.001, -0.002, 0.003};

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    WorldDrawItem ItemAt(const glm::dvec3 &meters, bool transparent = false)
    {
        WorldDrawItem item;
        item.pose = WorldPose{At(meters)};
        item.transparent = transparent;
        return item;
    }

    double MaxError(const glm::dvec3 &actual, const glm::dvec3 &expected)
    {
        const glm::dvec3 error = glm::abs(actual - expected);
        return std::max({error.x, error.y, error.z});
    }

    glm::dvec3 Translation(const DrawItem &item) { return glm::dvec3(glm::vec3(item.model[3])); }

    glm::vec3 Ndc(const glm::mat4 &viewProj, const glm::mat4 &model)
    {
        const glm::vec4 clip = viewProj * model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return glm::vec3(clip) / clip.w;
    }

    const mir::Camera kLens{60.0f, 0.1f, 100.0f};
}

TEST_CASE("A far draw keeps a millimeter offset from its viewport camera", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const std::vector<WorldDrawItem> world{ItemAt(kEdgeA + kStep)};
    std::vector<DrawItem> view;

    CHECK(mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), view) == 0);
    REQUIRE(view.size() == 1);
    CHECK(MaxError(Translation(view[0]), kStep) <= 1e-9);

    // narrowing both absolute positions first, as the Scene-origin path did,
    // loses the whole step
    CHECK(static_cast<float>(kEdgeA.x + kStep.x) - static_cast<float>(kEdgeA.x) == 0.0f);
}

TEST_CASE("Each viewport narrows against its own camera", "[renderer][lwc]")
{
    const glm::dvec3 nearA = kEdgeA + glm::dvec3(2.0, 0.0, -3.0);
    const glm::dvec3 nearB = kEdgeB + glm::dvec3(-1.0, 0.5, 4.0);
    const std::vector<WorldDrawItem> world{ItemAt(nearA), ItemAt(nearB)};
    const std::vector<WorldDrawItem> before = world;

    std::vector<DrawItem> fromA;
    std::vector<DrawItem> fromB;
    mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), fromA);
    mir::BuildViewDrawItems(world, At(kEdgeB), glm::mat4(1.0f), fromB);
    REQUIRE(fromA.size() == 2);
    REQUIRE(fromB.size() == 2);

    CHECK(MaxError(Translation(fromA[0]), glm::dvec3(2.0, 0.0, -3.0)) <= 1e-9);
    CHECK(MaxError(Translation(fromB[1]), glm::dvec3(-1.0, 0.5, 4.0)) <= 1e-9);
    // across the map, the float offset still lands within float's spacing
    CHECK(MaxError(Translation(fromA[1]), nearB - kEdgeA) <= 0.04);

    // the camera-relative value is submission-only: the world items are untouched
    for (size_t i = 0; i < world.size(); ++i)
        CHECK(world[i].pose.position.Meters() == before[i].pose.position.Meters());
}

TEST_CASE("A far camera projects like the same camera at the origin", "[renderer][lwc]")
{
    const glm::mat3 turn = glm::mat3_cast(glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::dvec3 offset{0.25, -0.5, -6.0};

    const glm::mat4 farViewProj = mir::MakeCameraRelativeViewProjection(WorldPose{At(kEdgeB), turn}, kLens, 1.5f);
    const glm::mat4 nearViewProj = mir::MakeCameraRelativeViewProjection(WorldPose{WorldPosition{}, turn}, kLens, 1.5f);
    CHECK(farViewProj == nearViewProj);

    std::vector<DrawItem> farView;
    std::vector<DrawItem> nearView;
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{ItemAt(kEdgeB + offset)}, At(kEdgeB), farViewProj, farView);
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{ItemAt(offset)}, WorldPosition{}, nearViewProj, nearView);
    REQUIRE(farView.size() == 1);
    REQUIRE(nearView.size() == 1);

    const glm::vec3 farNdc = Ndc(farViewProj, farView[0].model);
    const glm::vec3 nearNdc = Ndc(nearViewProj, nearView[0].model);
    CHECK(MaxError(glm::dvec3(farNdc), glm::dvec3(nearNdc)) <= 1e-6);
}

TEST_CASE("Far transparent draws sort back to front after the opaque ones", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // camera at A looking down -Z; two transparent draws 1 cm apart in depth,
    // well inside float's 1/64 m spacing at this X
    const WorldPose camera{At(kEdgeA)};

    std::vector<WorldDrawItem> world{
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -5.00), true),
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -9.00), false),
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -5.01), true),
    };
    world[0].tint.r = 0.0f; // nearer
    world[2].tint.r = 2.0f; // farther

    // the order is by view depth, so it holds with or without a far plane
    for (const mir::Camera &lens : {kLens, mir::Camera{60.0f, 0.1f, 0.0f}})
    {
        const glm::mat4 viewProj = mir::MakeCameraRelativeViewProjection(camera, lens, 1.0f);

        std::vector<DrawItem> view;
        mir::BuildViewDrawItems(world, camera.position, viewProj, view);
        REQUIRE(view.size() == 3);
        CHECK_FALSE(view[0].transparent);
        CHECK(view[1].tint.r == 2.0f);
        CHECK(view[2].tint.r == 0.0f);
    }
}

TEST_CASE("A draw beyond float range of the camera is left out", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const std::vector<WorldDrawItem> world{ItemAt(glm::dvec3(1e300, 0.0, 0.0)), ItemAt(kStep)};
    std::vector<DrawItem> view{DrawItem{}, DrawItem{}, DrawItem{}};

    CHECK(mir::BuildViewDrawItems(world, WorldPosition{}, glm::mat4(1.0f), view) == 1);
    REQUIRE(view.size() == 1);
    CHECK(MaxError(Translation(view[0]), kStep) <= 1e-9);
}

TEST_CASE("Submission carries the linear part and draw data through", "[renderer]")
{
    WorldDrawItem item = ItemAt(kEdgeB);
    item.pose.linear = glm::mat3(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.5f));
    item.normalMatrix = glm::mat3(glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 2.0f));
    item.tint = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    const glm::vec4 deformation[2]{};
    item.deformation = deformation;

    std::vector<DrawItem> view;
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{item}, At(kEdgeB), glm::mat4(1.0f), view);
    REQUIRE(view.size() == 1);
    CHECK(glm::mat3(view[0].model) == item.pose.linear);
    CHECK(view[0].model[3] == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(view[0].normalMatrix == item.normalMatrix);
    CHECK(view[0].tint == item.tint);
    CHECK(view[0].deformation.data() == deformation);
    CHECK(view[0].deformation.size() == 2);
}

TEST_CASE("Neighbouring tile draws meet on their shared edge after narrowing", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // Identity linear part, 256 m apart at the map edge.
    constexpr float kTile = 256.0f;
    constexpr float kGrid = 1.0f / 4096.0f;
    for (const float grid : {0.0f, kGrid})
    {
        INFO("translation grid " << grid);
        std::vector<WorldDrawItem> world{ItemAt(kEdgeA), ItemAt(kEdgeA + glm::dvec3(kTile, 0.0, 0.0)),
                                         ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, kTile))};
        for (WorldDrawItem &item : world)
            item.translationGrid = grid;

        int crossEdgeMismatches = 0;
        int alongEdgeMismatches = 0;
        double worstTranslation = 0.0;
        for (int step = 0; step < 200; ++step)
        {
            // None on float's 1/64 m grid out there.
            const glm::dvec3 offset{-600.0 + 6.37 * step, 1.7 + 0.91 * step, 590.0 - 5.83 * step};
            std::vector<DrawItem> view;
            REQUIRE(mir::BuildViewDrawItems(world, At(kEdgeA + offset), glm::mat4(1.0f), view) == 0);
            REQUIRE(view.size() == 3);
            worstTranslation = std::max(worstTranslation, MaxError(Translation(view[0]), -offset));

            const double ulp = std::ldexp(1.0, std::ilogb(glm::length(offset) + 2.0 * kTile) - 23);
            for (int s = 0; s <= 64; ++s)
            {
                const float along = 4.0f * float(s);
                const float height = 180.0f + 0.37f * float(s);
                // The x edge of draw 0 is draw 1's x = 0, and its z edge is draw 2's z = 0.
                const glm::vec4 a = view[0].model * glm::vec4(kTile, height, along, 1.0f);
                const glm::vec4 b = view[1].model * glm::vec4(0.0f, height, along, 1.0f);
                const glm::vec4 c = view[0].model * glm::vec4(along, height, kTile, 1.0f);
                const glm::vec4 d = view[2].model * glm::vec4(along, height, 0.0f, 1.0f);
                crossEdgeMismatches += (a.y != b.y || a.z != b.z || c.x != d.x || c.y != d.y) ? 1 : 0;
                alongEdgeMismatches += (a.x != b.x ? 1 : 0) + (c.z != d.z ? 1 : 0);
                CHECK(std::abs(double(a.x) - b.x) <= ulp);
                CHECK(std::abs(double(c.z) - d.z) <= ulp);
            }
        }
        CHECK(crossEdgeMismatches == 0);
        if (grid > 0.0f)
        {
            CHECK(alongEdgeMismatches == 0);
            CHECK(worstTranslation <= kGrid / 2.0);
        }
        else
        {
            // Each translation rounds in its own binade, so plain draws disagree.
            CHECK(alongEdgeMismatches > 0);
            CHECK(worstTranslation <= 1e-4);
        }
    }
}

TEST_CASE("A translation grid only applies within float's exact range", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    constexpr float kGrid = 1.0f / 4096.0f;
    // 2^23 steps of 1/4096 m is 2048 m.
    std::vector<WorldDrawItem> world{ItemAt(kEdgeA + glm::dvec3(2047.3, 0.0, 0.0)),
                                     ItemAt(kEdgeA + glm::dvec3(5000.3, 0.0, 0.0))};
    for (WorldDrawItem &item : world)
        item.translationGrid = kGrid;

    std::vector<DrawItem> view;
    REQUIRE(mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), view) == 0);
    REQUIRE(view.size() == 2);
    const float inside = view[0].model[3].x;
    CHECK(inside == float(std::round(2047.3 / kGrid) * kGrid));
    CHECK(view[1].model[3].x == float(5000.3));
}
