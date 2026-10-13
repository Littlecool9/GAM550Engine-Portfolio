/**
 * @file FrameProfile.h
 * @author Jackey Cen
 * @brief GPU timestamp samples for submitted renderer frames.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    struct RendererFrameTiming
    {
        uint64_t serial = 0;
        double gpuSeconds = 0.0;
        double uploadSeconds = 0.0;
        uint64_t primaryDrawCalls = 0;
        bool rayTracing = false;
    };

    struct RendererProfileInfo
    {
        std::string device;
        uint32_t driverVersion = 0;
        double timestampPeriodNanoseconds = 0.0;
        uint32_t timestampValidBits = 0;
    };
}
