#include <TerrainContinuity.h>
#include <TerrainGenerator.h>
#include <TerrainSeams.h>
#include <core/fs/FileIO.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;

int main(int argc, char **argv)
{
    if (argc != 3) { std::cerr << "usage: terrain_seam_experiment recipe out-directory\n"; return 2; }
    const auto parsed = LoadTerrainRecipe(argv[1]);
    if (!parsed.recipe || parsed.recipe->recipeVersion != 2) return 1;
    const std::filesystem::path out(argv[2]);
    std::filesystem::create_directories(out);
    std::string metrics = "case,weight,bandMultiplier,maxAdjustment,maxGridSlope,maxAddedSlope,maxCornerSlope,maxCornerAddedSlope,maxCornerAdjustment,maxSeamDifference,maxTolerance,breakpoints,probes,warnings,changedSamples,maxX,maxZ\n";
    std::string curves = "case,weight,bandMultiplier,curve,x,z,raw,final\n";
    for (const std::string name : {"direct-2", "direct-4", "direct-8", "mixed-four", "same-1-corner-8", "high-detail", "far-positive", "far-negative"})
    {
        TerrainRecipe recipe = *parsed.recipe;
        if (name.starts_with("direct"))
        {
            for (auto &r : recipe.sourceRules) r.spacing = 1.0;
            recipe.sourceRules[1].spacing = name == "direct-2" ? 2.0 : name == "direct-4" ? 4.0 : 8.0;
        }
        if (name == "same-1-corner-8")
        {
            for (auto &r : recipe.sourceRules) r.spacing = 1.0;
            recipe.sourceRules[3].spacing = 8.0;
        }
        if (name == "high-detail") recipe.relief.detail.amplitude = 500.0;
        if (name.starts_with("far"))
        {
            const int32_t shift = name == "far-positive" ? 586 : -586;
            auto bounds = recipe.layout.Bounds();
            const double offset = shift * recipe.layout.TileSize();
            bounds.minX += offset; bounds.maxX += offset;
            recipe.layout = *TerrainLayout::Create(recipe.layout.SampleSpacing(), recipe.layout.IntervalsPerTile(), bounds);
            for (auto &r : recipe.sourceRules) r.tile.x += shift;
            recipe.relief.tiltX = 0.0;
        }
        const TerrainGenerator generator(recipe);
        std::vector<TileHeights> raw;
        const auto range = recipe.layout.Tiles();
        for (int32_t z = range.min.z; z <= range.max.z; ++z) for (int32_t x = range.min.x; x <= range.max.x; ++x)
        {
            const auto layout = recipe.SourceLayout({x, z});
            if (!layout) return 1;
            auto tile = TileHeights::Create({x, z}, layout->IntervalsPerTile());
            if (!tile || !generator.FillTile(*layout, {x, z}, tile->Samples()).Ok()) return 1;
            raw.push_back(std::move(*tile));
        }
        const auto layers = recipe.SourceLayers();
        for (const auto weight : {TerrainSeamWeight::Linear, TerrainSeamWeight::Smoothstep})
            for (double multiplier : {1.0, 2.0, 4.0})
            {
                auto final = raw;
                TerrainReport report("experiment", {});
                if (!AdjustTerrainSeams(recipe, final, report, {multiplier, weight})) return 1;
                const auto stats = CheckMixedTileContinuity(recipe.layout, [&](TileCoord t) {
                    const auto it = std::find_if(final.begin(), final.end(), [&](const auto &v) { return v.Tile() == t; });
                    if (it == final.end()) return TerrainContinuityTile{};
                    const auto l = std::find_if(layers.begin(), layers.end(), [&](const auto &v) { return v.IntervalsPerTile() == it->IntervalsPerTile(); });
                    return TerrainContinuityTile{&*l, it->Samples()};
                }, report);
                if (!report.Ok()) { for (const auto &d : report.diagnostics) std::cerr << FormatDiagnostic(d) << '\n'; return 1; }
                double slope = 0.0, addedSlope = 0.0, cornerSlope = 0.0, cornerAdded = 0.0, cornerAdjustment = 0.0;
                const double cx = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(recipe.layout.Tiles().max.x));
                const double cz = recipe.layout.SamplePosition(recipe.layout.TileFirstSample(recipe.layout.Tiles().max.z));
                uint64_t changed = 0;
                for (size_t t = 0; t < final.size(); ++t)
                {
                    const auto &a = raw[t], &b = final[t];
                    const double s = *recipe.SourceSpacing(b.Tile());
                    const int32_t n = b.IntervalsPerTile();
                    const auto tileLayout = *recipe.SourceLayout(b.Tile());
                    for (int32_t k = 0; k <= n; ++k) for (int32_t i = 0; i <= n; ++i)
                    {
                        if (a.At(i, k) != b.At(i, k)) ++changed;
                        const double px = tileLayout.SamplePosition(tileLayout.TileFirstSample(b.Tile().x) + i);
                        const double pz = tileLayout.SamplePosition(tileLayout.TileFirstSample(b.Tile().z) + k);
                        const bool nearCorner = std::abs(px - cx) <= 16.0 && std::abs(pz - cz) <= 16.0;
                        if (nearCorner) cornerAdjustment = std::max(cornerAdjustment, std::abs(double(b.At(i, k)) - a.At(i, k)));
                        for (const auto [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}})
                        {
                            if (i + dx > n || k + dz > n) continue;
                            const double before = double(a.At(i + dx, k + dz)) - a.At(i, k);
                            const double after = double(b.At(i + dx, k + dz)) - b.At(i, k);
                            slope = std::max(slope, std::abs(after) / s);
                            addedSlope = std::max(addedSlope, std::abs(after - before) / s);
                            if (nearCorner && std::abs(px + dx * s - cx) <= 16.0 && std::abs(pz + dz * s - cz) <= 16.0)
                            {
                                cornerSlope = std::max(cornerSlope, std::abs(after) / s);
                                cornerAdded = std::max(cornerAdded, std::abs(after - before) / s);
                            }
                        }
                    }
                }
                const auto &maximum = *std::max_element(report.seams->adjustments.begin(), report.seams->adjustments.end(), [](const auto &a, const auto &b) { return a.maxAdjustment < b.maxAdjustment; });
                metrics += std::format("{},{},{},{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},{},{},{},{},{:.17g},{:.17g}\n",
                    name, report.seams->weight, multiplier, report.seams->maxAdjustment, slope, addedSlope, cornerSlope, cornerAdded, cornerAdjustment,
                    stats.maxSurfaceDifference, stats.maxTolerance, stats.breakpoints, stats.probes, report.warnings.size(), changed, maximum.x, maximum.z);
                const auto &a = raw.front(), &b = final.front();
                const auto layout = *recipe.SourceLayout(b.Tile());
                const auto sample = layout.TileFirstSample(b.Tile().x);
                const auto sampleZ = layout.TileFirstSample(b.Tile().z);
                const int32_t n = b.IntervalsPerTile();
                const auto &fineMaximum = report.seams->adjustments.front();
                const int32_t k = std::clamp<int32_t>(int32_t(std::llround((fineMaximum.z - layout.SamplePosition(sampleZ)) / layout.SampleSpacing())), 0, n);
                for (int32_t i = 0; i <= n; ++i)
                {
                    curves += std::format("{},{},{},cross-section,{:.17g},{:.17g},{:.17g},{:.17g}\n", name, report.seams->weight, multiplier,
                        layout.SamplePosition(sample + i), layout.SamplePosition(sampleZ + k), a.At(i, k), b.At(i, k));
                    curves += std::format("{},{},{},east-border,{:.17g},{:.17g},{:.17g},{:.17g}\n", name, report.seams->weight, multiplier,
                        layout.SamplePosition(sample + n), layout.SamplePosition(sampleZ + i), a.At(n, i), b.At(n, i));
                }
                if (multiplier == 2.0 && weight == TerrainSeamWeight::Smoothstep)
                    if (!WriteText(out / (name + "-report.json"), TerrainReportJson(report))) return 1;
            }
    }
    if (!WriteText(out / "metrics.csv", metrics) || !WriteText(out / "curves.csv", curves)) return 1;
    std::cout << metrics;
}
