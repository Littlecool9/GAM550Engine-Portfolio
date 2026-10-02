/**
 * @file TerrainSurface.cpp
 * @author Jackey Cen
 * @brief The canonical piecewise-linear surface over a release's final float32 samples.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainSurface.h"

#include "terrain/TileAddressing.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mir
{
    namespace
    {
        // Heights at (i, k), (i+1, k), (i, k+1), (i+1, k+1).
        struct CellCorners
        {
            double h00 = 0.0;
            double h10 = 0.0;
            double h01 = 0.0;
            double h11 = 0.0;
        };

        bool InBounds(const TerrainLocalBounds &bounds, double x, double z)
        {
            return x >= bounds.minX && x <= bounds.maxX && z >= bounds.minZ && z <= bounds.maxZ;
        }

        bool CellInTile(const TerrainLayout &layout, TileCoord tile, GlobalSampleIndex cell)
        {
            const int64_t i0 = layout.TileFirstSample(tile.x);
            const int64_t k0 = layout.TileFirstSample(tile.z);
            const int64_t n = layout.IntervalsPerTile();
            return cell.x >= i0 && cell.x < i0 + n && cell.z >= k0 && cell.z < k0 + n;
        }

        SurfaceStatus ReadCorners(const TerrainLayout &layout, const TileSamplesView &view, GlobalSampleIndex cell,
                                  CellCorners &out)
        {
            if (view.samples.empty())
                return SurfaceStatus::MissingTile;
            const size_t side = static_cast<size_t>(layout.IntervalsPerTile()) + 1;
            if (view.samples.size() != side * side)
                return SurfaceStatus::InvalidSamples;

            const size_t i = static_cast<size_t>(cell.x - layout.TileFirstSample(view.tile.x));
            const size_t k = static_cast<size_t>(cell.z - layout.TileFirstSample(view.tile.z));
            const float *row = view.samples.data() + k * side + i;
            out = {row[0], row[1], row[side], row[side + 1]};
            if (!std::isfinite(out.h00) || !std::isfinite(out.h10) || !std::isfinite(out.h01) ||
                !std::isfinite(out.h11))
                return SurfaceStatus::InvalidSamples;
            return SurfaceStatus::Ok;
        }

        // Operand order is part of the contract: shared edges agree bit for bit.
        void EvaluateTriangle(const CellCorners &h, double fx, double fz, double spacing, SurfaceSample &out)
        {
            if (fz >= fx)
            {
                out.triangle = 0;
                out.height = h.h00 + fz * (h.h01 - h.h00) + fx * (h.h11 - h.h01);
                out.normal = glm::normalize(glm::dvec3(h.h01 - h.h11, spacing, h.h00 - h.h01));
            }
            else
            {
                out.triangle = 1;
                out.height = h.h00 + fx * (h.h10 - h.h00) + fz * (h.h11 - h.h10);
                out.normal = glm::normalize(glm::dvec3(h.h00 - h.h10, spacing, h.h10 - h.h11));
            }
            out.status = SurfaceStatus::Ok;
        }

        SurfaceSample EvaluateCell(const TerrainLayout &layout, const TileSamplesView &view, GlobalSampleIndex cell,
                                   glm::dvec2 fraction)
        {
            SurfaceSample out;
            out.tile = view.tile;
            out.cell = cell;
            CellCorners corners;
            out.status = ReadCorners(layout, view, cell, corners);
            if (out.status == SurfaceStatus::Ok)
                EvaluateTriangle(corners, fraction.x, fraction.y, layout.SampleSpacing(), out);
            return out;
        }

        // The far edge reads exactly 1, so the non-owning side matches the owner.
        double CellFraction(const TerrainLayout &layout, int64_t cell, double coordinate)
        {
            if (coordinate == layout.SamplePosition(cell + 1))
                return 1.0;
            return (coordinate - layout.SamplePosition(cell)) / layout.SampleSpacing();
        }
    }

    CellTriangulation TriangulateCell(GlobalSampleIndex cell)
    {
        const GlobalSampleIndex c00 = cell;
        const GlobalSampleIndex c10{cell.x + 1, cell.z};
        const GlobalSampleIndex c01{cell.x, cell.z + 1};
        const GlobalSampleIndex c11{cell.x + 1, cell.z + 1};
        return {{c00, c11}, {{{c00, c01, c11}, {c00, c11, c10}}}};
    }

    SurfaceSample EvaluateSurface(const TerrainLayout &layout, TerrainLocalPosition local,
                                  const TileSampleLookup &lookup)
    {
        const TileAddress address = AddressLocal(layout, local);
        if (!address.IsInside())
            return {};

        if (!lookup)
        {
            SurfaceSample invalid;
            invalid.status = SurfaceStatus::InvalidInput;
            invalid.tile = address.tile;
            invalid.cell = address.cell.origin;
            return invalid;
        }
        const TileSamplesView view{address.tile, lookup(address.tile)};
        return EvaluateCell(layout, view, address.cell.origin, address.cell.fraction);
    }

    SurfaceSample EvaluateSurfaceInCell(const TerrainLayout &layout, const TileSamplesView &view,
                                        GlobalSampleIndex cell, TerrainLocalPosition local)
    {
        const glm::dvec3 meters = local.Meters();
        SurfaceSample out;
        out.tile = view.tile;
        out.cell = cell;

        if (!layout.Contains(view.tile) || !CellInTile(layout, view.tile, cell))
        {
            out.status = SurfaceStatus::InvalidInput;
            return out;
        }
        if (!InBounds(layout.Bounds(), meters.x, meters.z))
        {
            out.status = SurfaceStatus::OutsideMap;
            return out;
        }

        const bool onCell = meters.x >= layout.SamplePosition(cell.x) &&
                            meters.x <= layout.SamplePosition(cell.x + 1) &&
                            meters.z >= layout.SamplePosition(cell.z) && meters.z <= layout.SamplePosition(cell.z + 1);
        if (!onCell)
        {
            out.status = SurfaceStatus::InvalidInput;
            return out;
        }

        const glm::dvec2 fraction{CellFraction(layout, cell.x, meters.x), CellFraction(layout, cell.z, meters.z)};
        return EvaluateCell(layout, view, cell, fraction);
    }

    double SurfaceTolerance(double maxAbsHeight)
    {
        // Half a float ulp at magnitude m in [2^e, 2^(e+1)) is 2^(e - 24).
        const double magnitude = std::max(std::abs(maxAbsHeight), 1.0);
        return std::ldexp(1.0, std::ilogb(magnitude) - 24);
    }
}
