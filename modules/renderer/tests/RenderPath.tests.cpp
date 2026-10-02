/**
 * @file RenderPath.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/RenderPath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderPath.h>

#include <catch2/catch_test_macros.hpp>

using mir::ChooseRenderPath;
using mir::RasterFallback;
using mir::RenderPath;

TEST_CASE("One traceable view is ray traced", "[renderer][rt]")
{
    const mir::RenderPathChoice choice = ChooseRenderPath(true, true, false, 1);
    CHECK(choice.path == RenderPath::RayTraced);
    CHECK(choice.fallback == RasterFallback::None);
}

TEST_CASE("Without a ray tracing request every frame rasters silently", "[renderer][rt]")
{
    for (const bool supported : {false, true})
        for (const bool forced : {false, true})
            for (const size_t views : {size_t(0), size_t(1), size_t(3)})
            {
                const mir::RenderPathChoice choice = ChooseRenderPath(false, supported, forced, views);
                CHECK(choice.path == RenderPath::Raster);
                CHECK(choice.fallback == RasterFallback::None);
            }
}

TEST_CASE("A forced raster keeps the whole frame on raster with its own reason", "[renderer][rt]")
{
    for (const size_t views : {size_t(0), size_t(1), size_t(2)})
    {
        const mir::RenderPathChoice choice = ChooseRenderPath(true, true, true, views);
        CHECK(choice.path == RenderPath::Raster);
        CHECK(choice.fallback == RasterFallback::ForcedRaster);
    }

    // An unsupported device is the reason to report, forced or not.
    CHECK(ChooseRenderPath(true, false, true, 1).fallback == RasterFallback::Unsupported);
    CHECK(ChooseRenderPath(true, false, false, 1).fallback == RasterFallback::Unsupported);
}

TEST_CASE("Several views fall back to raster, no view has nothing to report", "[renderer][rt]")
{
    const mir::RenderPathChoice several = ChooseRenderPath(true, true, false, 2);
    CHECK(several.path == RenderPath::Raster);
    CHECK(several.fallback == RasterFallback::MultipleViews);

    const mir::RenderPathChoice none = ChooseRenderPath(true, true, false, 0);
    CHECK(none.path == RenderPath::Raster);
    CHECK(none.fallback == RasterFallback::None);
}
