/**
 * @file FixedTimestep.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/ecs/FixedTimestep substep counting, carry and cap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/FixedTimestep.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    // exact in binary, so every sum below is exact
    constexpr float kStep = 1.0f / 64.0f;

    FixedTimestep MakeTimestep()
    {
        return FixedTimestep{ .mStep = kStep, .mMaxSteps = 4 };
    }
}

TEST_CASE("FixedTimestep runs no step until a whole step has accumulated", "[ecs][timestep]")
{
    FixedTimestep timestep = MakeTimestep();

    REQUIRE(timestep.Advance(0.0f) == 0);
    REQUIRE(timestep.Advance(kStep * 0.5f) == 0);
    REQUIRE(timestep.mAccumulator == kStep * 0.5);
}

TEST_CASE("FixedTimestep carries the leftover to the next frame", "[ecs][timestep]")
{
    FixedTimestep timestep = MakeTimestep();

    REQUIRE(timestep.Advance(kStep * 2.5f) == 2);
    REQUIRE(timestep.mAccumulator == kStep * 0.5);

    // the half step left over completes a whole one
    REQUIRE(timestep.Advance(kStep * 0.5f) == 1);
    REQUIRE(timestep.mAccumulator == 0.0);
}

TEST_CASE("FixedTimestep caps substeps per frame and bounds the backlog", "[ecs][timestep]")
{
    FixedTimestep timestep = MakeTimestep();

    // a stall worth 16 steps
    REQUIRE(timestep.Advance(0.25f) == 4);

    // 12 steps are left, but only one capped frame of them carries
    REQUIRE(timestep.mAccumulator == kStep * 4.0);

    REQUIRE(timestep.Advance(0.0f) == 4);
    REQUIRE(timestep.mAccumulator == 0.0);
    REQUIRE(timestep.Advance(0.0f) == 0);
}
