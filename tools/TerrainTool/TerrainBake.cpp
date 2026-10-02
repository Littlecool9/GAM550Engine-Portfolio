/**
 * @file TerrainBake.cpp
 * @author Jackey Cen
 * @brief Bakes a terrain recipe into final tile heights, an encoded release and a candidate directory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBake.h"

#include "TerrainGenerator.h"
#include "TerrainValidate.h"

#include <assets/AssetPath.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

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
            }
            return "?";
        }

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        bool CheckArguments(const TerrainBakeRequest &request, const fs::path &out, std::string &assetPath,
                            TerrainReport &report)
        {
            using check::kArguments;
            if (request.recipePath.empty())
                Fail(report, kArguments, "--recipe", "no recipe file");

            const AssetPathResult root = NormalizeAssetPath(request.idRoot);
            const AssetPathResult logical = MakeLogicalAssetPath(request.idRoot, request.sourcePath);
            if (!root.Ok())
                Fail(report, kArguments, "--id-root", std::format("\"{}\": {}", request.idRoot, Describe(root.error)));
            else if (!logical.Ok())
                Fail(report, kArguments, "--source-path",
                     std::format("\"{}\": {}", request.sourcePath, Describe(logical.error)));
            else
                assetPath = logical.path;

            std::error_code ec;
            if (out.empty())
                Fail(report, kArguments, "--out", "no candidate directory");
            else if (fs::exists(out, ec))
            {
                if (!fs::is_directory(out, ec))
                    Fail(report, kArguments, "--out", std::format("{} exists and is not a directory", out.generic_string()));
                else if (!fs::is_empty(out, ec))
                    Fail(report, kArguments, "--out",
                         std::format("{} exists and is not empty; a bake never writes into or replaces an existing "
                                     "directory (remove the old candidate or choose another --out)",
                                     out.generic_string()));
            }
            if (report.Status(kArguments) == CheckStatus::Failed)
                return false;
            report.Ran(kArguments, std::format("asset {} ({})", assetPath, TerrainIdHex(MakeAssetId(assetPath).value)));
            return true;
        }
    }

    std::optional<std::vector<TileHeights>> BakeFinalHeights(const TerrainRecipe &recipe, TerrainReport &report)
    {
        const TerrainGenerator generator(recipe);
        const TerrainLayout &layout = generator.Layout();
        const TileRange range = layout.Tiles();
        std::vector<TileHeights> tiles;
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, layout.IntervalsPerTile());
                if (!tile)
                {
                    report.Add({std::string(check::kGenerate), "layout.intervalsPerTile", {{x, z}}, std::nullopt,
                                std::format("{} intervals per tile is beyond a tile block's limit {}",
                                            layout.IntervalsPerTile(), kMaxTerrainTileIntervals)});
                    return std::nullopt;
                }
                const SourceFillResult filled = generator.FillTile({x, z}, tile->Samples());
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
                   std::format("{} tiles of {}^2 samples from \"{}\" v{}; final heights = source heights (P1)",
                               tiles.size(), layout.IntervalsPerTile() + 1, recipe.generatorId,
                               recipe.generatorVersion));
        return tiles;
    }

    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainLayout &layout, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report)
    {
        const uint64_t heightsHash = TerrainHeightsHash(tiles);
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
        return TerrainBakedRelease{info, layout, nonce, std::move(tiles), std::move(*encoded.value)};
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
        manifest.files = DescribeReleaseFiles(baked.encoded);
        return manifest;
    }

    TerrainReport RunTerrainBake(const TerrainBakeRequest &request)
    {
        TerrainReport report("bake", {check::kArguments, check::kRecipe, check::kGenerate, check::kEncode,
                                      check::kWrite, check::kCandidate, check::kFormat, check::kIndex, check::kPacks,
                                      check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                                      check::kCorners, check::kIdentity});
        fs::path out = request.candidateDir.lexically_normal();
        if (!out.empty() && !out.has_filename())
            out = out.parent_path();
        report.candidateDir = out;
        report.recipePath = request.recipePath;

        std::string assetPath;
        if (!CheckArguments(request, out, assetPath, report))
            return report;
        report.assetPath = assetPath;

        RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
        for (RecipeIssue &issue : parsed.issues)
            report.Add({std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt, std::move(issue.message)});
        if (!parsed.recipe)
            return report;
        const TerrainRecipe &recipe = *parsed.recipe;
        const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
        report.recipeFingerprint = fingerprint;
        report.Ran(check::kRecipe, std::format("fingerprint {}", TerrainIdHex(fingerprint.value)));

        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, report);
        if (!tiles)
            return report;
        std::optional<TerrainBakedRelease> baked = EncodeBakedRelease(
            MakeAssetId(assetPath), fingerprint, recipe.layout, std::move(*tiles), request.options, report);
        if (!baked)
            return report;
        report.release = baked->info;
        report.heightsHash = baked->encoded.heightsHash;
        report.nonce = baked->nonce;
        report.layout = baked->layout;

        // Written beside the target so the rename stays on one volume.
        const fs::path staging =
            out.parent_path() / (out.filename().string() + "." + TerrainIdHex(baked->info.release.value) + ".partial");
        std::error_code ec;
        if (fs::exists(staging, ec))
        {
            Fail(report, check::kWrite, "--out",
                 std::format("{} already exists (left by an interrupted bake?); remove it", staging.generic_string()));
            return report;
        }
        fs::create_directories(staging, ec);
        std::string error;
        const TerrainCandidateManifest manifest =
            MakeCandidateManifest(*baked, assetPath, request.recipePath.generic_string());
        if (ec || !WriteCandidateFiles(staging, baked->encoded, manifest, error))
        {
            Fail(report, check::kWrite, "--out",
                 ec ? std::format("cannot create {} ({})", staging.generic_string(), ec.message()) : error);
            fs::remove_all(staging, ec);
            return report;
        }

        ValidateTerrainCandidate(staging, {&recipe, true}, report);
        if (!report.Ok())
        {
            fs::remove_all(staging, ec);
            report.Skip(check::kWrite, std::format("the written files failed validation; {} was removed and {} "
                                                   "not created",
                                                   staging.generic_string(), out.generic_string()));
            return report;
        }

        // An empty --out was allowed above; a directory rename cannot replace it.
        if (fs::exists(out, ec))
            fs::remove(out, ec);
        fs::rename(staging, out, ec);
        if (ec)
        {
            Fail(report, check::kWrite, "--out",
                 std::format("cannot rename {} to {} ({})", staging.generic_string(), out.generic_string(),
                             ec.message()));
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            return report;
        }
        uint64_t bytes = 0;
        for (const TerrainFileSummary &file : manifest.files)
            bytes += file.bytes;
        report.Ran(check::kWrite, std::format("{} release files ({} bytes) and {} in {}", manifest.files.size(), bytes,
                                              kCandidateManifestFileName, out.generic_string()));
        return report;
    }
}
