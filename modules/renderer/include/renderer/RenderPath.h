/**
 * @file RenderPath.h
 * @author Jackey Cen
 * @brief Whether a frame is ray traced or rastered, and why ray tracing was not used.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mir
{
    enum class RenderPath : uint8_t
    {
        Raster,
        RayTraced,
    };

    // None when ray tracing was not requested.
    enum class RasterFallback : uint8_t
    {
        None,
        Unsupported,
        // The scene holds something ray tracing cannot draw (terrain).
        ForcedRaster,
        MultipleViews,
    };

    struct RenderPathChoice
    {
        RenderPath path = RenderPath::Raster;
        RasterFallback fallback = RasterFallback::None;
    };

    // Whole frames only. Unsupported wins over forced raster, which wins over the view count.
    RenderPathChoice ChooseRenderPath(bool rayTracingRequested, bool rayTracingSupported, bool forceRaster,
                                      size_t viewCount);
}
