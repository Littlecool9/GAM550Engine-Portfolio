/**
 * @file TerrainBake.cpp
 * @author Jackey Cen
 * @brief Bakes a terrain recipe into final tile heights, an encoded release and a candidate directory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBake.h"
#include "TerrainDerivatives.h"
#include <unordered_map>

#include "TerrainGenerator.h"
#include "TerrainSeams.h"
#include "TerrainValidate.h"

#include <assets/AssetPath.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string_view SourceStatusName(SourceFillStatus status)
        {
            switch (status)
            {
            case SourceFillStatus::Ok:
                return "ok";
            case SourceFillStatus::OutsideMap:
                return "outside the map";
            case SourceFillStatus::EmptyRange:
                return "an empty range";
            case SourceFillStatus::WrongSize:
                return "a wrong block size";
            case SourceFillStatus::NonFinite:
                return "a non-finite height";
            case SourceFillStatus::InvalidGrid:
                return "a grid that does not nest in the reference grid";
            }
            return "?";
        }

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        // Removes the staging directory on every return and exception unless cleared after the rename.
        struct StagingCleanup
        {
            fs::path dir;

            ~StagingCleanup()
            {
                std::error_code ignored;
                if (!dir.empty())
                    fs::remove_all(dir, ignored);
            }
        };

        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool SameIgnoringCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
        }

        bool CheckArguments(const TerrainBakeRequest &request, const fs::path &out, std::string &assetPath,
                            TerrainReport &report)
        {
            using check::kArguments;
            if (const std::optional<TerrainAssetPath> asset =
                    CheckRecipeArguments(request.recipePath, request.idRoot, request.sourcePath, report))
                assetPath = asset->logicalPath;

            std::error_code ec;
            if (out.empty())
                Fail(report, kArguments, "--out", "no candidate directory");
            else if (fs::exists(out, ec))
            {
                if (!fs::is_directory(out, ec))
                    Fail(report, kArguments, "--out", std::format("{} exists and is not a directory", PathUtf8(out)));
                else if (!fs::is_empty(out, ec))
                    Fail(report, kArguments, "--out",
                         std::format("{} exists and is not empty; a bake never writes into or replaces an existing "
                                     "directory (remove the old candidate or choose another --out)",
                                     PathUtf8(out)));
            }
            if (report.Status(kArguments) == CheckStatus::Failed)
                return false;
            report.Ran(kArguments, std::format("asset {} ({})", assetPath, TerrainIdHex(MakeAssetId(assetPath).value)));
            return true;
        }
    }

    std::optional<std::string> RecipeSpellingMismatch(const fs::path &recipePath, std::string_view sourcePath)
    {
        const AssetPathResult normalized = NormalizeAssetPath(sourcePath);
        if (!normalized.Ok())
            return std::nullopt;
        const std::string_view wanted = std::string_view(normalized.path).substr(normalized.path.rfind('/') + 1);
        // The listing has the file's own spelling; the given path keeps the caller's.
        std::error_code ec;
        const fs::path parent = recipePath.has_parent_path() ? recipePath.parent_path() : fs::path(".");
        for (const fs::directory_entry &entry : fs::directory_iterator(parent, ec))
        {
            const std::string stored = PathUtf8(entry.path().filename());
            if (SameIgnoringCase(stored, PathUtf8(recipePath.filename())))
            {
                if (stored != wanted && SameIgnoringCase(stored, wanted))
                    return std::format("the recipe file is \"{}\" on disk but --source-path names \"{}\"; asset ids "
                                       "keep case, so this would publish an asset that a reference spelled like the "
                                       "file never finds (rename one to match)",
                                       stored, wanted);
                break;
            }
        }
        return std::nullopt;
    }

    std::optional<TerrainAssetPath> CheckRecipeArguments(const fs::path &recipePath, std::string_view idRoot,
                                                         std::string_view sourcePath, TerrainReport &report)
    {
        using check::kArguments;
        bool ok = true;
        if (recipePath.empty())
        {
            Fail(report, kArguments, "--recipe", "no recipe file");
            ok = false;
        }
        TerrainAssetPath asset = ResolveTerrainAssetPath(idRoot, sourcePath);
        if (!asset.Ok())
        {
            Fail(report, kArguments, asset.error == TerrainAssetPathError::IdRoot ? "--id-root" : "--source-path",
                 DescribeTerrainAssetPathError(asset, idRoot, sourcePath));
            return std::nullopt;
        }
        if (std::optional<std::string> mismatch = RecipeSpellingMismatch(recipePath, sourcePath))
        {
            Fail(report, kArguments, "--source-path", std::move(*mismatch));
            ok = false;
        }
        if (!ok)
            return std::nullopt;
        return asset;
    }

    std::optional<std::vector<TileHeights>> BakeFinalHeights(const TerrainRecipe &recipe, TerrainReport &report)
    {
        const TerrainGenerator generator(recipe);
        const TerrainLayout &layout = generator.Layout();
        const TileRange range = layout.Tiles();
        std::vector<TileHeights> tiles;
        report.sourceEstimates = recipe.SourceEstimates();
        for (int64_t tz = range.min.z; tz <= range.max.z; ++tz)
            for (int64_t tx = range.min.x; tx <= range.max.x; ++tx)
            {
                const int32_t x = static_cast<int32_t>(tx), z = static_cast<int32_t>(tz);
                const auto sourceLayout = recipe.SourceLayout({x, z});
                if (!sourceLayout)
                {
                    report.Add({std::string(check::kGenerate), "precision.tiles", {{x, z}}, std::nullopt,
                                "tile has no source spacing"});
                    return std::nullopt;
                }
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, sourceLayout->IntervalsPerTile());
                if (!tile)
                {
                    report.Add({std::string(check::kGenerate), "layout.intervalsPerTile", {{x, z}}, std::nullopt,
                                std::format("{} intervals per tile is beyond a tile block's limit {}",
                                            layout.IntervalsPerTile(), kMaxTerrainTileIntervals)});
                    return std::nullopt;
                }
                const SourceFillResult filled = generator.FillTile(*sourceLayout, {x, z}, tile->Samples());
                report.referenceSamples += filled.referenceSamples;
                report.peakDoubleSamples = std::max(report.peakDoubleSamples, filled.peakDoubleSamples);
                if (!filled.Ok())
                {
                    const bool atSample = filled.status == SourceFillStatus::NonFinite;
                    report.Add({std::string(atSample ? check::kFinite : check::kGenerate), "samples", {{x, z}},
                                atSample ? std::optional(filled.sample) : std::nullopt,
                                std::format("the generator returned {} for tile ({}, {})",
                                            SourceStatusName(filled.status), x, z)});
                    return std::nullopt;
                }
                tiles.push_back(std::move(*tile));
            }
        report.Ran(check::kGenerate,
                   std::format("{} source tiles from \"{}\" v{}; fixed reference {} m, tile {} m; {}",
                               tiles.size(), recipe.generatorId, recipe.generatorVersion,
                               layout.SampleSpacing(), layout.TileSize(), recipe.recipeVersion == 1 ?
                               "uniform P1 heights unchanged" : "filtered source grids"));
        if (!AdjustTerrainSeams(recipe, tiles, report)) return std::nullopt;
        return tiles;
    }

    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainLayout &layout, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report)
    {
        const uint64_t heightsHash = TerrainHeightsHash(layout, tiles);
        const uint64_t nonce = options.nonce ? *options.nonce : MakeTerrainReleaseNonce();
        const TerrainReleaseInfo info{asset, source, kTerrainFormatVersion,
                                      MintTerrainReleaseId(asset, source, heightsHash, nonce)};
        TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layout, tiles, options.pack);
        if (!encoded)
        {
            Fail(report, check::kEncode, "", encoded.error.message);
            return std::nullopt;
        }
        report.Ran(check::kEncode, std::format("release {}: index and {} packs, heights hash {}",
                                               TerrainIdHex(info.release.value), encoded.value->packs.size(),
                                               TerrainIdHex(encoded.value->heightsHash)));
        return TerrainBakedRelease{info, layout, nonce, std::move(tiles), std::move(*encoded.value), report.seams};
    }

    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainRecipe &recipe, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report)
    {
        if (recipe.recipeVersion != 1 && !report.seams)
        {
            Fail(report, check::kEncode, "seams", "v2 final heights require a seam summary");
            return std::nullopt;
        }
        if (report.seams && (report.seams->algorithmVersion != kTerrainSeamVersion ||
            report.seams->bandMultiplier != kTerrainSeamBandMultiplier || report.seams->weight != "smoothstep" || report.seams->warningThreshold != kTerrainSeamWarningThreshold))
        {
            Fail(report, check::kEncode, "seams", "prototype seam options cannot be encoded as a production candidate");
            return std::nullopt;
        }
        std::string derivativeError;
        auto derivatives=BakeTerrainDerivatives(recipe,tiles,derivativeError);
        if(!derivatives){Fail(report,check::kEncode,"derivatives",derivativeError);return {};}
        const auto &layers = derivatives->layers;
        std::vector<TerrainTileWrite> records;
        for (const auto &tile : tiles)
        {
            const auto spacing = recipe.SourceSpacing(tile.Tile());
            const auto layer = std::find_if(layers.begin(), layers.end(), [&](const auto &layout) {
                return spacing && layout.SampleSpacing() == *spacing;
            });
            if (layer == layers.end())
            {
                Fail(report, check::kEncode, "precision.tiles", "tile has no assigned source layer");
                return std::nullopt;
            }
            records.push_back({&tile, static_cast<uint32_t>(layer - layers.begin()), true, 0.0});
        }
        std::unordered_map<TileCoord,uint64_t> sourceChecksums;
        for(const auto &tile:tiles)sourceChecksums.emplace(tile.Tile(),TerrainFinalHeightChecksum(tile));
        for(size_t i=0;i<derivatives->queryHeights.size();++i)
            records.push_back({&derivatives->queryHeights[i],derivatives->queryLayers[i],false,derivatives->queryErrors[i],
                sourceChecksums.at(derivatives->queryHeights[i].Tile())});
        const uint64_t heightsHash = TerrainPayloadsHash(layers, records, derivatives->payloads);
        const uint64_t nonce = options.nonce ? *options.nonce : MakeTerrainReleaseNonce();
        const TerrainReleaseInfo info{asset, source, kTerrainFormatVersion,
                                      MintTerrainReleaseId(asset, source, heightsHash, nonce)};
        auto encoded = EncodeTerrainRelease(info, layers, records, derivatives->payloads, options.pack);
        if (!encoded)
        {
            Fail(report, check::kEncode, "", encoded.error.message);
            return std::nullopt;
        }
        report.Ran(check::kEncode, std::format("release {}: {} source tiles, {} sparse layers, {} packs",
            TerrainIdHex(info.release.value), tiles.size(), layers.size(), encoded.value->packs.size()));
        return TerrainBakedRelease{info, recipe.layout, nonce, std::move(tiles), std::move(*encoded.value), report.seams};
    }

    TerrainCandidateManifest MakeCandidateManifest(const TerrainBakedRelease &baked, std::string assetPath,
                                                   std::string recipePath)
    {
        TerrainCandidateManifest manifest;
        manifest.assetPath = std::move(assetPath);
        manifest.recipePath = std::move(recipePath);
        manifest.release = baked.info;
        manifest.nonce = baked.nonce;
        manifest.heightsHash = baked.encoded.heightsHash;
        manifest.formatMinor = kTerrainFormatMinor;
        manifest.contentVersion = kTerrainHeightsContentVersion;
        manifest.surfaceVersion = kTerrainSurfaceVersion;
        const auto index = ParseTerrainIndex(baked.encoded.index);
        manifest.layerCount = static_cast<uint32_t>(index.value->layers.size());
        manifest.payloadCount = static_cast<uint32_t>(index.value->tiles.size());
        manifest.files = DescribeReleaseFiles(baked.encoded);
        manifest.seams = baked.seams;
        return manifest;
    }

    bool BakeTerrainCandidate(const TerrainBakeRequest &request, TerrainReport &report)
    {
        fs::path out = request.candidateDir.lexically_normal();
        if (!out.empty() && !out.has_filename())
            out = out.parent_path();
        report.candidateDir = out;
        report.recipePath = request.recipePath;

        std::string assetPath;
        if (!CheckArguments(request, out, assetPath, report))
            return false;
        report.assetPath = assetPath;

        RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
        for (RecipeIssue &issue : parsed.issues)
            report.Add({std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt, std::move(issue.message)});
        if (!parsed.recipe)
            return false;
        const TerrainRecipe &recipe = *parsed.recipe;
        const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
        report.recipeFingerprint = fingerprint;
        report.Ran(check::kRecipe, std::format("fingerprint {}", TerrainIdHex(fingerprint.value)));

        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        if (!tiles)
            return false;
        std::optional<TerrainBakedRelease> baked = EncodeBakedRelease(
            MakeAssetId(assetPath), fingerprint, recipe, std::move(*tiles), request.options, report);
        if (!baked)
            return false;
        report.release = baked->info;
        report.heightsHash = baked->encoded.heightsHash;
        report.nonce = baked->nonce;
        report.layout = baked->layout;

        // Written beside the target so the rename stays on one volume.
        fs::path staging = out;
        staging += "." + TerrainIdHex(baked->info.release.value) + ".partial";
        std::error_code ec;
        if (fs::exists(staging, ec))
        {
            Fail(report, check::kWrite, "--out",
                 std::format("{} already exists (left by an interrupted bake?); remove it", PathUtf8(staging)));
            return false;
        }
        fs::create_directories(staging, ec);
        StagingCleanup cleanup{staging};
        std::string error;
        const TerrainCandidateManifest manifest =
            MakeCandidateManifest(*baked, assetPath, PathUtf8(request.recipePath));
        if (ec || !WriteCandidateFiles(staging, baked->encoded, manifest, error))
        {
            Fail(report, check::kWrite, "--out",
                 ec ? std::format("cannot create {} ({})", PathUtf8(staging), ec.message()) : error);
            return false;
        }

        ValidateTerrainCandidate(staging, {&recipe, true}, report);
        if (!report.Ok())
        {
            report.Skip(check::kWrite, std::format("the written files failed validation; {} was removed and {} "
                                                   "not created",
                                                   PathUtf8(staging), PathUtf8(out)));
            return false;
        }

        // An empty --out was allowed above; a directory rename cannot replace it.
        if (fs::exists(out, ec))
            fs::remove(out, ec);
        fs::rename(staging, out, ec);
        if (ec)
        {
            Fail(report, check::kWrite, "--out",
                 std::format("cannot rename {} to {} ({})", PathUtf8(staging), PathUtf8(out),
                             ec.message()));
            return false;
        }
        cleanup.dir.clear();
        uint64_t bytes = 0;
        for (const TerrainFileSummary &file : manifest.files)
            bytes += file.bytes;
        report.Ran(check::kWrite, std::format("{} release files ({} bytes) and {} in {}", manifest.files.size(), bytes,
                                              kCandidateManifestFileName, PathUtf8(out)));
        return report.Ok();
    }

    TerrainReport RunTerrainBake(const TerrainBakeRequest &request)
    {
        TerrainReport report("bake", {check::kArguments, check::kRecipe, check::kGenerate, check::kEncode,
                                      check::kWrite, check::kCandidate, check::kFormat, check::kIndex, check::kPacks,
                                      check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                                      check::kCorners, check::kIdentity});
        BakeTerrainCandidate(request, report);
        return report;
    }
}
