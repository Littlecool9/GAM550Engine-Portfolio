/**
 * @file RenderPath.cpp
 * @author Jackey Cen
 * @brief Whether a frame is ray traced or rastered, and why ray tracing was not used.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/RenderPath.h"

namespace mir
{
    RenderPathChoice ChooseRenderPath(bool rayTracingRequested, bool rayTracingSupported, bool forceRaster,
                                      size_t viewCount)
    {
        if (!rayTracingRequested)
            return {};
        if (!rayTracingSupported)
            return {RenderPath::Raster, RasterFallback::Unsupported};
        if (forceRaster)
            return {RenderPath::Raster, RasterFallback::ForcedRaster};
        if (viewCount > 1)
            return {RenderPath::Raster, RasterFallback::MultipleViews};
        if (viewCount == 0)
            return {};
        return {RenderPath::RayTraced, RasterFallback::None};
    }
}
