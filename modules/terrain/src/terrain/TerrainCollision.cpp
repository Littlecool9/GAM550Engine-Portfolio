#include <terrain/TerrainCollision.h>
#include <terrain/TerrainLod.h>
#include <terrain/TileAddressing.h>
#include "PackInternal.h"
#include "AxisLocate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace mir
{
    namespace
    {
        bool BinarySpacing(double spacing)
        {
            int exponent = 0;
            return std::isfinite(spacing) && spacing > 0 && std::frexp(spacing, &exponent) == 0.5;
        }

        bool ExactSumLessEqual(double a, double b, double c, double d)
        {
            // Keep the addition's rounding remainder: an outward-rounded endpoint
            // must not prove coverage, including in a double world far from zero.
            const auto sum = [](double x, double y) {
                const double value = x + y;
                if (!std::isfinite(value)) return std::array{value, 0.0};
                const double part = value - x;
                return std::array{value, (x - (value - part)) + (y - part)};
            };
            const auto left = sum(a, b), right = sum(c, d);
            return std::isfinite(left[0]) && std::isfinite(right[0]) &&
                (left[0] < right[0] || (left[0] == right[0] && left[1] <= right[1]));
        }

        std::optional<double> RefinementDeviation(const TerrainLayout &sourceLayout, const TileHeights &source,
            const TerrainLayout &uniform, const TileHeights &block)
        {
            const double ratio = sourceLayout.SampleSpacing() / uniform.SampleSpacing();
            if (!BinarySpacing(sourceLayout.SampleSpacing()) || !BinarySpacing(uniform.SampleSpacing()) ||
                !BinarySpacing(ratio) || ratio < 1 || ratio > kMaxTerrainTileIntervals ||
                source.Tile() != block.Tile() || uniform.IntervalsPerTile() != block.IntervalsPerTile() ||
                source.IntervalsPerTile() != sourceLayout.IntervalsPerTile()) return {};
            const int factor = static_cast<int>(ratio), sn = source.IntervalsPerTile();
            if (int64_t(sn) * factor != uniform.IntervalsPerTile()) return {};
            const auto tile = source.Tile();
            const auto sx = sourceLayout.TileFirstSample(tile.x), sz = sourceLayout.TileFirstSample(tile.z);
            const auto ux = uniform.TileFirstSample(tile.x), uz = uniform.TileFirstSample(tile.z);
            for (int i = 0; i <= sn; ++i)
                if (sourceLayout.SamplePosition(sx+i) != uniform.SamplePosition(ux+int64_t(i)*factor) ||
                    sourceLayout.SamplePosition(sz+i) != uniform.SamplePosition(uz+int64_t(i)*factor)) return {};
            const double ox = sourceLayout.SamplePosition(sx), oz = sourceLayout.SamplePosition(sz);
            const auto padded = TerrainLayout::Create(sourceLayout.SampleSpacing(), sn,
                {ox, oz, sourceLayout.SamplePosition(sx+sn), sourceLayout.SamplePosition(sz+sn)});
            if (!padded) return {};
            double maxHeight = 1, bound = 0;
            for (float h : source.Samples()) { if (!std::isfinite(h)) return {}; maxHeight = std::max(maxHeight, std::abs(double(h))); }
            const TileSampleLookup lookup = [&source, tile](TileCoord requested) {
                return requested == tile ? source.Samples() : std::span<const float>();
            };
            const int n = uniform.IntervalsPerTile();
            // Exact binary nesting keeps every fine triangle inside a source triangle.
            for (int k = 0; k <= n; ++k)
                for (int i = 0; i <= n; ++i)
                {
                    const float h = block.Samples()[size_t(k)*(n+1)+i];
                    if (!std::isfinite(h)) return {};
                    maxHeight = std::max(maxHeight, std::abs(double(h)));
                    const auto point = TerrainLocalPosition::FromMeters({uniform.SamplePosition(ux+i),0,
                                                                         uniform.SamplePosition(uz+k)});
                    if (!point) return {};
                    const auto sampled = EvaluateSurface(*padded, *point, lookup);
                    if (!sampled.IsOk()) return {};
                    bound = std::max(bound, std::abs(sampled.height-double(h)));
                }
            const double margin = 4096*std::numeric_limits<double>::epsilon()*maxHeight*
                (1+sourceLayout.TileSize()/sourceLayout.SampleSpacing()+
                 std::max(std::abs(ox),std::abs(oz))/sourceLayout.SampleSpacing());
            return std::isfinite(bound+margin) ? std::optional(std::nextafter(bound+margin,
                std::numeric_limits<double>::infinity())) : std::nullopt;
        }
    }

    bool ValidTerrainCollisionPolicy(const TerrainCollisionPolicy &policy)
    {
        if (!std::isfinite(policy.maxError) || policy.maxError < 0 || policy.spacings.empty()) return false;
        double previous = std::numeric_limits<double>::infinity();
        for (double spacing : policy.spacings)
        {
            if (!std::isfinite(spacing) || spacing <= 0 || spacing >= previous) return false;
            previous = spacing;
        }
        return true;
    }

    TerrainResult<TerrainUniformSamples> BuildTerrainUniformSamples(const TerrainIndex &index,
        const TerrainHeightQuery &query, double spacing)
    {
        if (query.Release() != index.release)
            return detail::Fail<TerrainUniformSamples>(TerrainPackErrorCode::WrongIdentity,
                                                       "the resident query belongs to another release");
        const TerrainSourceTileLookup sources = [&query](uint32_t layer, TileCoord tile)
        { return query.ResidentTile(layer, tile); };
        return BuildTerrainUniformSamples(index, sources, spacing, index.layout.Bounds());
    }

    TerrainResult<TerrainUniformSamples> BuildTerrainUniformSamples(const TerrainIndex &index,
        const TerrainSourceTileLookup &sources, double spacing, const TerrainLocalBounds &region)
    {
        using detail::Fail;
        using Code = TerrainPackErrorCode;
        const auto uniform = TerrainLayout::CreateFromTileSize(index.layout.TileSize(), spacing, region);
        if (!uniform)
            return Fail<TerrainUniformSamples>(Code::Unsupported, "{} m does not form an exact tile grid", spacing);
        if (!(static_cast<float>(spacing) > 0) || !std::isfinite(static_cast<float>(spacing)))
            return Fail<TerrainUniformSamples>(Code::Unsupported, "spacing does not fit a positive float");
        const auto range = uniform->Tiles();
        const auto bounds = uniform->Bounds();
        const int64_t firstX = detail::LocateAxis(*uniform, bounds.minX, bounds.maxX).cell;
        const int64_t firstZ = detail::LocateAxis(*uniform, bounds.minZ, bounds.maxZ).cell;
        const int64_t lastX = detail::LocateAxis(*uniform, bounds.maxX, bounds.maxX).cell + 1;
        const int64_t lastZ = detail::LocateAxis(*uniform, bounds.maxZ, bounds.maxZ).cell + 1;
        if (lastX - firstX + 1 > kMaxTerrainAreaSamplesPerSide || lastZ - firstZ + 1 > kMaxTerrainAreaSamplesPerSide)
            return Fail<TerrainUniformSamples>(Code::Unsupported, "{} m exceeds the single height-field side limit", spacing);

        if (!std::isfinite(static_cast<float>(spacing) * static_cast<float>(std::max(lastX-firstX, lastZ-firstZ))))
            return Fail<TerrainUniformSamples>(Code::Unsupported, "uniform shape extent does not fit a float");
        std::unordered_map<TileCoord, TileHeights> blocks;
        TerrainUniformSamples result;
        result.validBounds = bounds;
        const int n = uniform->IntervalsPerTile();
        for (int64_t tz = range.min.z; tz <= range.max.z; ++tz)
            for (int64_t tx = range.min.x; tx <= range.max.x; ++tx)
            {
                const int32_t x = static_cast<int32_t>(tx), z = static_cast<int32_t>(tz);
                const TileCoord tile{x, z};
                const auto *record = index.Find(tile);
                if (!record || record->layer >= index.layers.size())
                    return Fail<TerrainUniformSamples>(Code::Corrupt, "tile ({}, {}) has no unique source", x, z);
                const auto source = sources(record->layer, tile);
                if (!source)
                    return Fail<TerrainUniformSamples>(Code::NotFound, "tile ({}, {}) has no resident final source", x, z);
                const auto &layout = index.layers[record->layer];
                const int64_t sx = layout.TileFirstSample(x), sz = layout.TileFirstSample(z);
                const double x0 = layout.SamplePosition(sx), z0 = layout.SamplePosition(sz);
                const double x1 = layout.SamplePosition(sx + layout.IntervalsPerTile());
                const double z1 = layout.SamplePosition(sz + layout.IntervalsPerTile());
                if (uniform->SamplePosition(uniform->TileFirstSample(x)) != x0 ||
                    uniform->SamplePosition(uniform->TileFirstSample(z)) != z0 ||
                    uniform->SamplePosition(uniform->TileFirstSample(x) + n) != x1 ||
                    uniform->SamplePosition(uniform->TileFirstSample(z) + n) != z1)
                    return Fail<TerrainUniformSamples>(Code::Unsupported, "{} m changes a source tile edge", spacing);
                const auto padded = TerrainLayout::Create(layout.SampleSpacing(), layout.IntervalsPerTile(), {x0, z0, x1, z1});
                if (!padded)
                    return Fail<TerrainUniformSamples>(Code::Corrupt, "source tile padding is not addressable");
                const TileSampleLookup lookup = [source, tile](TileCoord requested) {
                    return requested == tile ? source->Samples() : std::span<const float>();
                };
                auto block = TileHeights::Create(tile, n);
                if (!block)
                    return Fail<TerrainUniformSamples>(Code::Unsupported, "uniform tile exceeds the sample limit");
                for (int k = 0; k <= n; ++k)
                    for (int i = 0; i <= n; ++i)
                    {
                        const auto point = TerrainLocalPosition::FromMeters({
                            uniform->SamplePosition(uniform->TileFirstSample(x) + i), 0.0,
                            uniform->SamplePosition(uniform->TileFirstSample(z) + k)});
                        const auto sampled = point ? EvaluateSurface(*padded, *point, lookup) : SurfaceSample{};
                        if (!sampled.IsOk() || !std::isfinite(static_cast<float>(sampled.height)))
                            return Fail<TerrainUniformSamples>(Code::Corrupt, "source padding has no finite height");
                        block->Set(i, k, static_cast<float>(sampled.height));
                    }
                const int sn = source->IntervalsPerTile();
                const int side = sn + 1;
                const auto heights = source->Samples();
                for (int k = 0; k < sn; ++k)
                    for (int i = 0; i < sn; ++i)
                    {
                        const double a = heights[size_t(k)*side+i], b = heights[size_t(k)*side+i+1];
                        const double d = heights[size_t(k+1)*side+i], c = heights[size_t(k+1)*side+i+1];
                        result.maxAbsHeight = std::max({result.maxAbsHeight, std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
                        result.maxSlope = std::max({result.maxSlope,
                            (std::abs(c-d)+std::abs(d-a))/layout.SampleSpacing(),
                            (std::abs(b-a)+std::abs(c-b))/layout.SampleSpacing()});
                    }
                blocks.emplace(tile, std::move(*block));
            }
        // The larger tile owns each shared node, including four-tile corners.
        for (auto &[tile, block] : blocks)
            for (int k = 0; k <= n; ++k)
                for (int i = 0; i <= n; ++i)
                {
                    const TileCoord owner{tile.x + (i == n && tile.x < range.max.x),
                                          tile.z + (k == n && tile.z < range.max.z)};
                    if (owner != tile)
                        block.Set(i, k, blocks.at(owner).Samples()[size_t(owner.z != tile.z ? 0 : k)*(n+1)+(owner.x != tile.x ? 0 : i)]);
                }
        for (const auto &[tile, block] : blocks)
        {
            const auto values = block.Samples();
            for (int k = 0; k < n; ++k)
                for (int i = 0; i < n; ++i)
                {
                    const double a = values[size_t(k)*(n+1)+i], b = values[size_t(k)*(n+1)+i+1];
                    const double d = values[size_t(k+1)*(n+1)+i], c = values[size_t(k+1)*(n+1)+i+1];
                    result.maxSlope = std::max({result.maxSlope,
                        (std::abs(c-d)+std::abs(d-a))/spacing, (std::abs(b-a)+std::abs(c-b))/spacing});
                }
            const auto *record = index.Find(tile);
            const auto source = sources(record->layer, tile);
            auto deviation = RefinementDeviation(index.layers[record->layer], *source, *uniform, block);
            if (!deviation) deviation = TerrainRegularDeviation(index.layers[record->layer], *source, *uniform, block);
            if (!deviation)
                return Fail<TerrainUniformSamples>(Code::Corrupt, "uniform triangle error cannot be enclosed");
            result.maxDeviation = std::max(result.maxDeviation, *deviation);
        }
        const TileSampleLookup lookup = [&blocks](TileCoord tile) {
            const auto found = blocks.find(tile);
            return found == blocks.end() ? std::span<const float>() : found->second.Samples();
        };
        auto area = BuildTerrainAreaSamples(*uniform, index.release, lookup);
        if (!area) return {std::nullopt, area.error};
        result.area = std::move(*area.value);
        return {std::move(result), {}};
    }

    bool TerrainCollisionCovers(const TerrainUniformSamples &samples, const TerrainPlacement &placement,
        WorldPosition origin, double extentX, double extentZ)
    {
        if (placement.asset != samples.area.release.asset || !std::isfinite(extentX) ||
            !std::isfinite(extentZ) || extentX <= 0 || extentZ <= 0) return false;
        const auto at = origin.Meters();
        const glm::dvec3 translation(placement.translation.Meters());
        const auto &bounds = samples.validBounds;
        return ExactSumLessEqual(double(at.x), 0.0, translation.x, bounds.minX) &&
            ExactSumLessEqual(double(at.z), 0.0, translation.z, bounds.minZ) &&
            ExactSumLessEqual(translation.x, bounds.maxX, double(at.x), extentX) &&
            ExactSumLessEqual(translation.z, bounds.maxZ, double(at.z), extentZ);
    }

    std::optional<TerrainCollisionError> TerrainCollisionErrorBudget(const TerrainUniformSamples &samples,
        const TerrainPlacement &placement, WorldPosition origin, double quantizationError, double horizontalError)
    {
        if (samples.area.countX < 2 || samples.area.countZ < 2 || !(samples.area.spacing > 0) ||
            !std::isfinite(samples.area.spacing) || samples.maxAbsHeight < 0 ||
            placement.asset != samples.area.release.asset || !std::isfinite(quantizationError) ||
            !std::isfinite(horizontalError) || quantizationError < 0 || horizontalError < 0 ||
            !std::isfinite(samples.maxDeviation) || samples.maxDeviation < 0 ||
            !std::isfinite(samples.maxSlope) || samples.maxSlope < 0 || !std::isfinite(samples.maxAbsHeight))
            return {};
        const glm::dvec3 local = samples.area.origin.Meters();
        const glm::dvec3 translation(placement.translation.Meters());
        const glm::dvec3 at(origin.Meters());
        const glm::dvec3 rounding = glm::abs(at - (translation + local));
        const double extent = samples.area.spacing * std::max(samples.area.countX - 1, samples.area.countZ - 1);
        const double worldXZ = std::max(std::abs(at.x), std::abs(at.z)) + extent;
        const double eps = std::numeric_limits<WorldReal>::epsilon();
        const double worldHorizontal = std::max(rounding.x, rounding.z) + 2 * eps * std::max(1.0, worldXZ);
        const double worldVertical = rounding.y + 2 * eps * std::max(1.0, std::abs(at.y) + samples.maxAbsHeight);
        TerrainCollisionError error;
        error.geometry = samples.maxDeviation;
        error.quantization = quantizationError;
        const double scaleDrift = std::abs(double(static_cast<float>(samples.area.spacing)) - samples.area.spacing) *
            std::max(samples.area.countX - 1, samples.area.countZ - 1);
        error.horizontal = (horizontalError + scaleDrift + worldHorizontal) * samples.maxSlope;
        error.numeric = worldVertical + 64 * std::numeric_limits<double>::epsilon() * std::max(1.0, samples.maxAbsHeight);
        error.total = std::nextafter(error.geometry + error.quantization + error.horizontal + error.numeric,
                                    std::numeric_limits<double>::infinity());
        return std::isfinite(error.total) ? std::optional(error) : std::nullopt;
    }
}
