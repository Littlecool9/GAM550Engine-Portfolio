/**
 * @file FixedTimestep.h
 * @author Jackey Cen
 * @brief Accumulator that turns frame time into fixed-rate substeps
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/log/Assert.h"

#include <algorithm>
#include <cstdint>

namespace mir
{
    // Drains frame time in fixed steps for SystemPhase::FixedUpdate.
    struct FixedTimestep
    {
        float mStep = 1.0f / 60.0f; // seconds per substep
        uint32_t mMaxSteps = 4;     // per frame, so a stall cannot spiral
        double mAccumulator = 0.0;  // seconds not yet stepped

        // Adds dt and returns the substeps to run this frame, at most mMaxSteps.
        uint32_t Advance(float dt)
        {
            MIR_ASSERT(mStep > 0.0f && mMaxSteps > 0, "FixedTimestep::Advance: step and max steps must be positive");

            mAccumulator += dt;

            uint32_t steps = 0;
            while (steps < mMaxSteps && mAccumulator >= mStep)
            {
                mAccumulator -= mStep;
                ++steps;
            }

            // leftover carries, but at most one capped frame of catch-up
            mAccumulator = std::min(mAccumulator, static_cast<double>(mStep) * mMaxSteps);
            return steps;
        }
    };
}
