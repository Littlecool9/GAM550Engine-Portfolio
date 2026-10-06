#include "TerrainSeams.h"
#include "TerrainGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        struct Edge
        {
            double spacing = 0.0;
            double width = 0.0;
            std::vector<float> values;
        };

        double Weight(double distance, double width, TerrainSeamWeight weight)
        {
            const double t = std::clamp(1.0 - distance / width, 0.0, 1.0);
            return weight == TerrainSeamWeight::Linear ? t : t * t * (3.0 - 2.0 * t);
        }

        double EdgeHeight(const Edge &edge, int32_t sample, int32_t n)
        {
            const int32_t coarseN = static_cast<int32_t>(edge.values.size()) - 1;
            const int32_t ratio = n / coarseN;
            if (sample == n) return edge.values.back();
            const int32_t cell = sample / ratio;
            const double t = double(sample % ratio) / ratio;
            return double(edge.values[cell]) + (double(edge.values[cell + 1]) - edge.values[cell]) * t;
        }
    }

    void ReportSeamWarnings(TerrainReport &report)
    {
        if (!report.seams) return;
        for (const auto &a : report.seams->adjustments)
            if (a.maxAdjustment > kTerrainSeamWarningThreshold)
            {
                std::vector<TileCoord> named{a.tile};
                named.insert(named.end(), a.relatedTiles.begin(), a.relatedTiles.end());
                report.Warn({"seams", a.border, std::move(named), std::nullopt,
                    std::format("QUALITY WARNING: final height adjustment {} m exceeds {} m at ({}, {}) m; band {} m",
                        a.maxAdjustment, kTerrainSeamWarningThreshold, a.x, a.z, a.bandWidth)});
            }
    }

    bool AdjustTerrainSeams(const TerrainRecipe &recipe, std::span<TileHeights> tiles,
                            TerrainReport &report, TerrainSeamOptions options)
    {
        if (recipe.recipeVersion == 1) return true;
        if (!std::isfinite(options.bandMultiplier) || options.bandMultiplier <= 0.0 ||
            (options.weight != TerrainSeamWeight::Linear && options.weight != TerrainSeamWeight::Smoothstep))
        {
            report.Add({"seams", "bandMultiplier", {}, std::nullopt, "band multiplier must be finite and positive"});
            return false;
        }
        const TerrainLayout &reference = recipe.layout;
        const TerrainGenerator generator(recipe);
        std::map<std::pair<int32_t, int32_t>, const TileHeights *> raw;
        for (const auto &tile : tiles)
        {
            const auto layout = recipe.SourceLayout(tile.Tile());
            if (!layout || layout->IntervalsPerTile() != tile.IntervalsPerTile() ||
                !raw.emplace(std::pair{tile.Tile().z, tile.Tile().x}, &tile).second ||
                std::any_of(tile.Samples().begin(), tile.Samples().end(), [](float h) { return !std::isfinite(h); }))
            {
                report.Add({"seams", "samples", {tile.Tile()}, std::nullopt, "invalid source tile for seam adjustment"});
                return false;
            }
        }
        const TileRange range = reference.Tiles();
        const uint64_t expected = (int64_t(range.max.x) - range.min.x + 1) * (int64_t(range.max.z) - range.min.z + 1);
        if (raw.size() != expected)
        {
            report.Add({"seams", "tiles", {}, std::nullopt, "seam adjustment requires every source tile"});
            return false;
        }
        std::map<std::pair<int32_t, int32_t>, float> corners;
        auto corner = [&](int32_t x, int32_t z) {
            const std::pair key{z, x};
            if (const auto it = corners.find(key); it != corners.end()) return it->second;
            double spacing = reference.SampleSpacing();
            for (int32_t dz : {-1, 0}) for (int32_t dx : {-1, 0})
                if (const auto s = recipe.SourceSpacing({x + dx, z + dz})) spacing = std::max(spacing, *s);
            const auto layout = TerrainLayout::CreateFromTileSize(reference.TileSize(), spacing, reference.Bounds());
            const float h = generator.SampleHeight(*layout, {layout->TileFirstSample(x), layout->TileFirstSample(z)});
            corners.emplace(key, h);
            return h;
        };

        // Snapshot only the boundary curves before any tile is changed.
        struct TileEdges { std::array<Edge, 4> edges; std::array<float, 4> corners; };
        std::map<std::pair<int32_t, int32_t>, TileEdges> boundaries;
        for (const auto &tile : tiles)
        {
            const TileCoord t = tile.Tile();
            const double sourceSpacing = *recipe.SourceSpacing(t);
            TileEdges b;
            b.corners = {corner(t.x, t.z), corner(t.x + 1, t.z), corner(t.x, t.z + 1), corner(t.x + 1, t.z + 1)};
            const TileCoord neighbours[4] = {{t.x - 1, t.z}, {t.x + 1, t.z}, {t.x, t.z - 1}, {t.x, t.z + 1}};
            for (int e = 0; e < 4; ++e)
            {
                Edge &edge = b.edges[e];
                const auto neighbourSpacing = recipe.SourceSpacing(neighbours[e]);
                edge.spacing = std::max(sourceSpacing, neighbourSpacing.value_or(sourceSpacing));
                edge.width = std::min(reference.TileSize() * 0.5, options.bandMultiplier * edge.spacing);
                const TileHeights *owner = &tile;
                if (neighbourSpacing && *neighbourSpacing > sourceSpacing)
                    owner = raw.at({neighbours[e].z, neighbours[e].x});
                const int32_t ownerN = owner->IntervalsPerTile();
                edge.values.resize(ownerN + 1);
                const bool other = owner != &tile;
                for (int32_t a = 0; a <= ownerN; ++a)
                {
                    if (e == 0) edge.values[a] = owner->At(other ? ownerN : 0, a);
                    if (e == 1) edge.values[a] = owner->At(other ? 0 : ownerN, a);
                    if (e == 2) edge.values[a] = owner->At(a, other ? ownerN : 0);
                    if (e == 3) edge.values[a] = owner->At(a, other ? 0 : ownerN);
                }
                edge.values.front() = b.corners[e < 2 ? e : (e == 2 ? 0 : 2)];
                edge.values.back() = b.corners[e < 2 ? e + 2 : (e == 2 ? 1 : 3)];
            }
            boundaries.emplace(std::pair{t.z, t.x}, std::move(b));
        }
        TerrainSeamSummary summary{kTerrainSeamVersion, options.bandMultiplier, kTerrainSeamWarningThreshold};
        summary.weight = options.weight == TerrainSeamWeight::Smoothstep ? "smoothstep" : "linear";
        static constexpr std::string_view names[4] = {"left", "right", "bottom", "top"};
        for (auto &tile : tiles)
        {
            const TileCoord t = tile.Tile();
            const int32_t n = tile.IntervalsPerTile();
            const TerrainLayout layout = *recipe.SourceLayout(t);
            const auto &b = boundaries.at({t.z, t.x});
            std::array<std::vector<double>, 4> deltas;
            for (auto &d : deltas) d.resize(n + 1);
            for (int32_t a = 0; a <= n; ++a)
            {
                deltas[0][a] = EdgeHeight(b.edges[0], a, n) - tile.At(0, a);
                deltas[1][a] = EdgeHeight(b.edges[1], a, n) - tile.At(n, a);
                deltas[2][a] = EdgeHeight(b.edges[2], a, n) - tile.At(a, 0);
                deltas[3][a] = EdgeHeight(b.edges[3], a, n) - tile.At(a, n);
            }
            const double c[4] = {double(b.corners[0]) - tile.At(0, 0), double(b.corners[1]) - tile.At(n, 0),
                double(b.corners[2]) - tile.At(0, n), double(b.corners[3]) - tile.At(n, n)};
            TerrainSeamAdjustment maximum{t};
            for (int32_t k = 0; k <= n; ++k) for (int32_t i = 0; i <= n; ++i)
            {
                const double x = i * layout.SampleSpacing(), z = k * layout.SampleSpacing();
                const double w[4] = {Weight(x, b.edges[0].width, options.weight),
                    Weight((n - i) * layout.SampleSpacing(), b.edges[1].width, options.weight),
                    Weight(z, b.edges[2].width, options.weight),
                    Weight((n - k) * layout.SampleSpacing(), b.edges[3].width, options.weight)};
                const float before = tile.At(i, k);
                double height = double(before) + w[0] * deltas[0][k] + w[1] * deltas[1][k] +
                    w[2] * deltas[2][i] + w[3] * deltas[3][i] -
                    w[0] * w[2] * c[0] - w[1] * w[2] * c[1] - w[0] * w[3] * c[2] - w[1] * w[3] * c[3];
                if (i == 0) height = EdgeHeight(b.edges[0], k, n);
                if (i == n) height = EdgeHeight(b.edges[1], k, n);
                if (k == 0) height = EdgeHeight(b.edges[2], i, n);
                if (k == n) height = EdgeHeight(b.edges[3], i, n);
                const float final = static_cast<float>(height);
                if (!std::isfinite(final))
                {
                    report.Add({"seams", "samples", {t}, std::nullopt, "seam adjustment produced a non-finite height"});
                    return false;
                }
                tile.Set(i, k, final);
                const double change = std::abs(double(final) - before);
                if (change > maximum.maxAdjustment)
                {
                    const double cornerTerms[4] = {w[0] * w[2] * c[0], w[1] * w[2] * c[1],
                        w[0] * w[3] * c[2], w[1] * w[3] * c[3]};
                    const double contributions[4] = {w[0] * deltas[0][k] - 0.5 * (cornerTerms[0] + cornerTerms[2]),
                        w[1] * deltas[1][k] - 0.5 * (cornerTerms[1] + cornerTerms[3]),
                        w[2] * deltas[2][i] - 0.5 * (cornerTerms[0] + cornerTerms[1]),
                        w[3] * deltas[3][i] - 0.5 * (cornerTerms[2] + cornerTerms[3])};
                    int e = 0;
                    for (int q = 1; q < 4; ++q) if (std::abs(contributions[q]) > std::abs(contributions[e])) e = q;
                    maximum.relatedTiles.clear();
                    auto related = [&](TileCoord other) {
                        if (other != t && recipe.SourceSpacing(other)) maximum.relatedTiles.push_back(other);
                    };
                    const TileCoord neighbours[4] = {{t.x - 1, t.z}, {t.x + 1, t.z}, {t.x, t.z - 1}, {t.x, t.z + 1}};
                    for (int q = 0; q < 4; ++q) if (contributions[q] != 0.0) related(neighbours[q]);
                    for (int q = 0; q < 4; ++q) if (cornerTerms[q] != 0.0)
                    {
                        const int32_t cx = t.x + (q % 2), cz = t.z + (q / 2);
                        for (int32_t dz : {-1, 0}) for (int32_t dx : {-1, 0}) related({cx + dx, cz + dz});
                    }
                    std::sort(maximum.relatedTiles.begin(), maximum.relatedTiles.end(), [](TileCoord a, TileCoord b) {
                        return std::pair{a.z, a.x} < std::pair{b.z, b.x};
                    });
                    maximum.relatedTiles.erase(std::unique(maximum.relatedTiles.begin(), maximum.relatedTiles.end()), maximum.relatedTiles.end());
                    maximum.border = names[e];
                    maximum.maxAdjustment = change;
                    maximum.bandWidth = b.edges[e].width;
                    maximum.x = layout.SamplePosition(layout.TileFirstSample(t.x) + i);
                    maximum.z = layout.SamplePosition(layout.TileFirstSample(t.z) + k);
                }
            }
            summary.maxAdjustment = std::max(summary.maxAdjustment, maximum.maxAdjustment);
            summary.adjustments.push_back(std::move(maximum));
        }
        std::sort(summary.adjustments.begin(), summary.adjustments.end(), [](const auto &a, const auto &b) {
            return std::pair{a.tile.z, a.tile.x} < std::pair{b.tile.z, b.tile.x};
        });
        report.seams = std::move(summary);
        ReportSeamWarnings(report);
        report.Ran("seams", std::format("final samples adjusted; max {} m, smooth finite band, version {}",
            report.seams->maxAdjustment, kTerrainSeamVersion));
        return true;
    }
}
