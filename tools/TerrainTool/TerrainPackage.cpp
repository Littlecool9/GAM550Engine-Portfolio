/**
 * @file TerrainPackage.cpp
 * @author Jackey Cen
 * @brief Copies an asset's current release into the game directory a build without the editor reads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPackage.h"

#include "TerrainSync.h"
#include "TerrainValidate.h"

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <format>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::initializer_list<std::string_view> kAfterCurrent{
            check::kCandidate, check::kFormat, check::kIndex,    check::kPacks,  check::kCoverage,
            check::kPayloads,  check::kFinite, check::kEdges,    check::kCorners, check::kRegions,
            check::kIdentity,  check::kTarget, check::kCopy,     check::kVerify, check::kRename,
            check::kPointer};
        constexpr std::initializer_list<std::string_view> kAfterSource{
            check::kTarget, check::kCopy, check::kVerify, check::kRename, check::kPointer, check::kPrune};

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        void SkipNotRun(TerrainReport &report, std::initializer_list<std::string_view> checks, const std::string &why)
        {
            for (std::string_view name : checks)
                for (const TerrainCheck &c : report.checks)
                    if (c.name == name && c.status == CheckStatus::Skipped && c.detail == "not run")
                        report.Skip(name, why);
        }

        std::string Join(const std::vector<std::string> &items)
        {
            std::string text;
            for (const std::string &item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text;
        }

        // Keeps current, the release it names and the held lock.
        void Prune(const fs::path &assetDir, TerrainReleaseId keep, TerrainReport &report)
        {
            const std::string keepHex = TerrainIdHex(keep.value);
            std::vector<fs::path> extra;
            std::error_code ec;
            for (const fs::directory_entry &entry : fs::directory_iterator(assetDir, ec))
            {
                const std::string name = PathUtf8(entry.path().filename());
                if (name != kTerrainCurrentPointerFileName && name != keepHex && name != kTerrainPublishLockFileName)
                    extra.push_back(entry.path());
            }
            std::vector<std::string> removed;
            std::vector<std::string> left;
            for (const fs::path &path : extra)
            {
                std::error_code removeEc;
                fs::remove_all(path, removeEc);
                if (removeEc)
                    left.push_back(std::format("{} ({})", PathUtf8(path.filename()), removeEc.message()));
                else
                    removed.push_back(PathUtf8(path.filename()));
            }
            // Not a failure: only current is read, and the next package retries.
            std::string detail = std::format("kept current and {}; removed {}", keepHex,
                                             removed.empty() ? std::string("nothing") : Join(removed));
            if (!left.empty())
                detail += "; still there until the next package: " + Join(left);
            report.Ran(check::kPrune, detail);
        }
    }

    TerrainReport RunTerrainPackage(const TerrainPackageRequest &request)
    {
        TerrainReport report("package", {check::kArguments, check::kCurrent, check::kLock, check::kCandidate,
                                         check::kFormat, check::kIndex, check::kPacks, check::kCoverage,
                                         check::kPayloads, check::kFinite, check::kEdges, check::kCorners,
                                         check::kRegions, check::kIdentity, check::kTarget, check::kCopy,
                                         check::kVerify, check::kRename, check::kPointer, check::kPrune});
        const fs::path source = request.gameDir.empty() ? fs::path() : TerrainReleasesRoot(request.gameDir);
        const fs::path out = request.outDir.empty() ? fs::path() : TerrainReleasesRoot(request.outDir);
        report.publish = TerrainPublishSummary{out};

        const TerrainAssetPath asset = ResolveTerrainAssetPath(request.idRoot, request.sourcePath);
        if (!asset.Ok())
            Fail(report, check::kArguments, asset.error == TerrainAssetPathError::IdRoot ? "--id-root" : "--source-path",
                 DescribeTerrainAssetPathError(asset, request.idRoot, request.sourcePath));
        if (request.gameDir.empty())
            Fail(report, check::kArguments, "--game-dir", "no game directory");
        if (request.outDir.empty())
            Fail(report, check::kArguments, "--out", "no output game directory");
        else if (!source.empty() && (IsSameOrInside(out, source) || IsSameOrInside(source, out)))
            Fail(report, check::kArguments, "--out",
                 std::format("{} and the game's releases tree {} must not contain each other: package removes old "
                             "releases from its output",
                             PathUtf8(out), PathUtf8(source)));
        if (report.Status(check::kArguments) == CheckStatus::Failed)
            return report;
        report.assetPath = asset.logicalPath;
        report.Ran(check::kArguments, std::format("asset {} ({}), from {} to {}", asset.logicalPath,
                                                  TerrainIdHex(asset.asset.value), PathUtf8(source),
                                                  PathUtf8(out)));

        const AssetId id = asset.asset;
        std::string problem;
        const std::optional<TerrainReleaseId> release = PeekTerrainCurrent(source, id, &problem);
        if (!release)
        {
            Fail(report, check::kCurrent, std::string(kTerrainCurrentPointerFileName),
                 std::format("in the game's releases: {}; build the game (or run sync) so its recipe is published "
                             "first",
                             problem));
            report.publish->current = PeekTerrainCurrent(out, id);
            return report;
        }
        const std::string releaseHex = TerrainIdHex(release->value);

        const fs::path assetDir = TerrainAssetReleasesDir(out, id);
        std::string error;
        std::optional<TerrainPublishLock> lock = TerrainPublishLock::Acquire(assetDir, request.lockWait, error);
        if (!lock)
        {
            Fail(report, check::kLock, std::string(kTerrainPublishLockFileName), error);
            report.publish->current = PeekTerrainCurrent(out, id);
            return report;
        }
        report.Ran(check::kLock, PathUtf8(assetDir / kTerrainPublishLockFileName));

        std::optional<TerrainReleaseId> previous;
        std::optional<std::string> mismatch;
        const TerrainResult<TerrainReleaseHandle> opened = OpenTerrainRelease(source, id, *release);
        if (!opened)
        {
            previous = PeekTerrainCurrent(out, id);
            mismatch = std::format("the game's release does not open: {}", opened.error.message);
        }
        else
        {
            mismatch = TerrainCurrentMismatch(out, id, opened.value->Index().release.source, previous);
            if (!mismatch && previous != release)
                mismatch = std::format("the output's current names release {}", TerrainIdHex(previous->value));
        }
        report.publish->previous = previous;
        if (!mismatch)
        {
            const TerrainIndex &index = opened.value->Index();
            report.release = index.release;
            report.formatMinor = index.formatMinor;
            report.contentVersion = index.contentVersion;
            report.heightsHash = index.heightsHash;
            report.layout = index.layout;
            report.publish->current = previous;
            const std::string why = std::format("release {} is current in the game and in the output", releaseHex);
            report.Ran(check::kCurrent, why + "; nothing to copy");
            SkipNotRun(report, kAfterCurrent, "not needed: " + why);
            Prune(assetDir, *release, report);
            return report;
        }
        report.Ran(check::kCurrent, std::format("copying release {}: {}", releaseHex, *mismatch));

        const fs::path sourceDir = TerrainReleaseDir(source, id, *release);
        report.candidateDir = sourceDir;
        ValidateTerrainCandidate(sourceDir, {nullptr, false}, report);
        if (request.fault && request.fault->step == TerrainPublishStep::Candidate)
            Fail(report, check::kCandidate, "", "test fault after the source check");
        // No candidate.json or recipe to compare; current is the reference.
        const bool named = report.release && report.release->asset == id && report.release->release == *release;
        if (report.Ok() && !named)
            Fail(report, check::kIdentity, std::string(kTerrainIndexFileName),
                 std::format("{} is not release {} of asset {}", PathUtf8(sourceDir), releaseHex,
                             TerrainIdHex(id.value)));
        else if (report.Ok())
            report.Ran(check::kIdentity, std::format("{} is release {} of asset {}, as current names",
                                                     kTerrainIndexFileName, releaseHex, TerrainIdHex(id.value)));
        if (!report.Ok())
        {
            SkipNotRun(report, kAfterSource, "the game's release did not validate; the output was not touched");
            report.publish->current = PeekTerrainCurrent(out, id);
            return report;
        }

        TerrainReleaseInstall install;
        install.sourceDir = sourceDir;
        install.files = report.files;
        install.release = *report.release;
        install.releasesRoot = out;
        install.recordedBy = "the game's release";
        // A directory of this id that did not match is a damaged copy.
        install.replaceExisting = true;
        install.fault = request.fault;
        const bool installed = InstallTerrainRelease(install, nullptr, report);
        report.publish->current = PeekTerrainCurrent(out, id);
        if (installed)
            Prune(assetDir, *release, report);
        else
            SkipNotRun(report, {check::kPrune}, "the copy did not complete; nothing was removed");
        return report;
    }
}
