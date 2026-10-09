/**
 * @file TerrainSync.cpp
 * @author Jackey Cen
 * @brief Publishes a recipe only when the current release no longer matches it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainSync.h"

#include "TerrainCandidate.h"

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <format>
#include <initializer_list>
#include <system_error>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::initializer_list<std::string_view> kAfterCurrent{
            check::kGenerate, check::kEncode,   check::kWrite,  check::kCandidate, check::kFormat,
            check::kIndex,    check::kPacks,    check::kCoverage, check::kPayloads, check::kFinite,
            check::kEdges,    check::kCorners,  check::kIdentity, check::kTarget,  check::kCopy,
            check::kVerify,   check::kRename,   check::kPointer};

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        void SkipAll(TerrainReport &report, std::initializer_list<std::string_view> checks, const std::string &why)
        {
            for (std::string_view c : checks)
                if (report.Status(c) != CheckStatus::Failed)
                    report.Skip(c, why);
        }

        // Only an empty directory or candidate.json plus candidate files; a release never has candidate.json.
        bool RemoveOwnCandidate(const fs::path &dir, std::string &error)
        {
            std::error_code ec;
            if (!fs::exists(dir, ec))
                return true;
            if (!fs::is_directory(dir, ec))
            {
                error = std::format("{} exists and is not a directory", PathUtf8(dir));
                return false;
            }
            bool empty = true;
            bool hasManifest = false;
            for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
            {
                const std::string name = PathUtf8(entry.path().filename());
                empty = false;
                hasManifest = hasManifest || name == kCandidateManifestFileName;
                const bool candidateFile = name == kTerrainIndexFileName || name == kCandidateManifestFileName ||
                                           (name.starts_with("chunk_") && name.ends_with(".tpack"));
                if (!entry.is_regular_file(ec) || !candidateFile)
                {
                    error = std::format("{} holds {}, so it is not a TerrainTool candidate; sync only replaces its "
                                        "own candidate (choose another --candidate)",
                                        PathUtf8(dir), name);
                    return false;
                }
            }
            if (!empty && !hasManifest)
            {
                error = std::format("{} has no {}, so it is not a TerrainTool candidate; sync only replaces its own "
                                    "candidate (choose another --candidate)",
                                    PathUtf8(dir), kCandidateManifestFileName);
                return false;
            }
            fs::remove_all(dir, ec);
            if (ec)
                error = std::format("cannot remove the old candidate {} ({})", PathUtf8(dir), ec.message());
            return !ec;
        }
    }

    std::optional<std::string> TerrainCurrentMismatch(const fs::path &releasesRoot, AssetId asset,
                                                      TerrainSourceFingerprint fingerprint,
                                                      std::optional<TerrainReleaseId> &current,
                                                      std::optional<TerrainReleaseHandle> *opened)
    {
        std::string problem;
        current = PeekTerrainCurrent(releasesRoot, asset, &problem);
        if (!current)
            return problem;
        const std::string id = TerrainIdHex(current->value);
        const fs::path dir = TerrainReleaseDir(releasesRoot, asset, *current);
        std::error_code ec;
        // So a missing release is a reason, not a logged read error.
        if (!fs::is_regular_file(dir / kTerrainIndexFileName, ec))
            return std::format("current names release {}, but {} has no {}", id, PathUtf8(dir),
                               kTerrainIndexFileName);
        const TerrainResult<TerrainReleaseHandle> release = OpenTerrainRelease(releasesRoot, asset, *current);
        if (!release)
            return std::format("current release {} does not open: {}", id, release.error.message);
        if (opened)
            *opened = *release.value;

        const TerrainIndex &index = release.value->Index();
        if (index.release.source != fingerprint)
            return std::format("current release {} was baked from recipe fingerprint {}; the recipe is now {}", id,
                               TerrainIdHex(index.release.source.value), TerrainIdHex(fingerprint.value));
        if (index.release.format != kTerrainFormatVersion || index.formatMinor != kTerrainFormatMinor ||
            index.contentVersion != kTerrainHeightsContentVersion)
            return std::format("current release {} is format {}.{} content {}; this tool writes {}.{} content {}", id,
                               index.release.format.value, index.formatMinor, index.contentVersion,
                               kTerrainFormatVersion.value, kTerrainFormatMinor, kTerrainHeightsContentVersion);
        // Format 2.1 can also describe generic Height-only releases. Those are
        // readable, but cannot satisfy the runtime's default source-layer mesh.
        for (const TerrainTileRecord &source : index.tiles)
            if (source.source &&
                (!index.Find(source.tile, source.layer, TerrainPayloadKind::LodHeight, 0) ||
                 !index.Find(source.tile, source.layer, TerrainPayloadKind::Normal, 0)))
                return std::format("current release {}: tile ({}, {}) has no source-layer LOD/normal derivatives",
                                   id, source.tile.x, source.tile.z);
        for (const TerrainPackRecord &pack : index.packs)
        {
            const std::string name = TerrainPackFileName(pack.chunk);
            const uintmax_t size = fs::file_size(dir / name, ec);
            if (ec || size != pack.fileSize)
                return std::format("current release {}: {} is {}, the index says {} bytes", id, name,
                                   ec ? std::string("missing") : std::format("{} bytes", size), pack.fileSize);
        }
        return std::nullopt;
    }

    TerrainReport RunTerrainSync(const TerrainSyncRequest &request)
    {
        TerrainReport report("sync", {check::kArguments, check::kRecipe, check::kLock, check::kCurrent,
                                      check::kGenerate, check::kEncode, check::kWrite, check::kCandidate,
                                      check::kFormat, check::kIndex, check::kPacks, check::kCoverage,
                                      check::kPayloads, check::kFinite, check::kEdges, check::kCorners,
                                      check::kIdentity, check::kTarget, check::kCopy, check::kVerify,
                                      check::kRename, check::kPointer});
        report.recipePath = request.recipePath;
        report.candidateDir = request.candidateDir;
        const fs::path releases = request.gameDir.empty() ? fs::path() : TerrainReleasesRoot(request.gameDir);
        report.publish = TerrainPublishSummary{releases};

        const std::optional<TerrainAssetPath> asset =
            CheckRecipeArguments(request.recipePath, request.idRoot, request.sourcePath, report);
        if (request.gameDir.empty())
            Fail(report, check::kArguments, "--game-dir", "no game directory");
        if (request.candidateDir.empty())
            Fail(report, check::kArguments, "--candidate", "no candidate directory");
        else if (!releases.empty() &&
                 (IsSameOrInside(request.candidateDir, releases) || IsSameOrInside(releases, request.candidateDir)))
            Fail(report, check::kArguments, "--candidate",
                 std::format("{} and the releases tree {} must not contain each other: sync deletes its old "
                             "candidate",
                             PathUtf8(request.candidateDir), PathUtf8(releases)));
        if (!asset || report.Status(check::kArguments) == CheckStatus::Failed)
            return report;
        report.assetPath = asset->logicalPath;
        report.Ran(check::kArguments,
                   std::format("asset {} ({}), releases {}", asset->logicalPath, TerrainIdHex(asset->asset.value),
                               PathUtf8(releases)));

        RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
        for (RecipeIssue &issue : parsed.issues)
            report.Add({std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt, std::move(issue.message)});
        if (!parsed.recipe)
            return report;
        const TerrainRecipe &recipe = *parsed.recipe;
        const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
        report.recipeFingerprint = fingerprint;
        report.Ran(check::kRecipe, std::format("fingerprint {}", TerrainIdHex(fingerprint.value)));

        // Held until the pointer switch, so a second sync sees this one's result.
        std::string error;
        std::optional<TerrainPublishLock> lock =
            TerrainPublishLock::Acquire(TerrainAssetReleasesDir(releases, asset->asset), request.lockWait, error);
        if (!lock)
        {
            Fail(report, check::kLock, std::string(kTerrainPublishLockFileName), error);
            return report;
        }
        report.Ran(check::kLock, PathUtf8(lock->AssetDir() / kTerrainPublishLockFileName));

        std::optional<TerrainReleaseId> current;
        std::optional<TerrainReleaseHandle> opened;
        const std::optional<std::string> mismatch =
            TerrainCurrentMismatch(releases, asset->asset, fingerprint, current, &opened);
        report.publish->previous = current;
        if (!mismatch)
        {
            const TerrainIndex &index = opened->Index();
            report.release = index.release;
            report.formatMinor = index.formatMinor;
            report.contentVersion = index.contentVersion;
            report.heightsHash = index.heightsHash;
            report.layout = index.layout;
            report.publish->current = current;
            const std::string why = std::format("release {} is current and was baked from this recipe",
                                                TerrainIdHex(current->value));
            report.Ran(check::kCurrent, why + "; nothing to publish");
            SkipAll(report, kAfterCurrent, "not needed: " + why);
            return report;
        }
        report.Ran(check::kCurrent, "publishing: " + *mismatch);

        if (!RemoveOwnCandidate(request.candidateDir, error))
        {
            Fail(report, check::kWrite, "--candidate", error);
            return report;
        }
        TerrainBakeRequest bake;
        bake.recipePath = request.recipePath;
        bake.idRoot = request.idRoot;
        bake.sourcePath = request.sourcePath;
        bake.candidateDir = request.candidateDir;
        bake.options = request.options;
        if (!BakeTerrainCandidate(bake, report))
        {
            report.publish->current = PeekTerrainCurrent(releases, asset->asset);
            return report;
        }

        TerrainPublishRequest publish;
        publish.candidateDir = request.candidateDir;
        publish.releasesRoot = releases;
        publish.fault = request.fault;
        PublishTerrainCandidate(publish, &recipe, &*lock, report);
        return report;
    }
}
