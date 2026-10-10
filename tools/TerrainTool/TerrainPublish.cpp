/**
 * @file TerrainPublish.cpp
 * @author Jackey Cen
 * @brief Publishes a validated candidate as an immutable release and switches the current pointer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPublish.h"

#include "TerrainCandidate.h"
#include "TerrainValidate.h"

#include <core/fs/FileIO.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace std::chrono_literals;

        // Antivirus, indexers or a reader of current can block a rename on Windows for a moment.
        constexpr int kMoveAttempts = 40;
        constexpr auto kMoveRetryDelay = 50ms;

        constexpr std::initializer_list<std::string_view> kPublishChecks{
            check::kLock, check::kTarget, check::kCopy, check::kVerify, check::kRename, check::kPointer};
        constexpr std::initializer_list<std::string_view> kInstallChecks{
            check::kTarget, check::kCopy, check::kVerify, check::kRename, check::kPointer};

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

        std::string LastErrorMessage()
        {
#ifdef _WIN32
            return std::system_category().message(static_cast<int>(GetLastError()));
#else
            return std::strerror(errno);
#endif
        }

        // So a rename that publishes the file cannot outrun its bytes.
        bool FlushToDisk(const fs::path &path)
        {
#ifdef _WIN32
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            const bool flushed = FlushFileBuffers(file) != 0;
            CloseHandle(file);
            return flushed;
#else
            const int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0)
                return false;
            const bool flushed = fsync(fd) == 0;
            close(fd);
            return flushed;
#endif
        }

        // replace=false fails if to exists.
        bool Move(const fs::path &from, const fs::path &to, bool replace, std::string &error)
        {
#ifdef _WIN32
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
            for (int attempt = 1;; ++attempt)
            {
                if (MoveFileExW(from.c_str(), to.c_str(), flags))
                    return true;
                const DWORD code = GetLastError();
                if ((code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) || attempt == kMoveAttempts)
                {
                    error = std::system_category().message(static_cast<int>(code));
                    return false;
                }
                std::this_thread::sleep_for(kMoveRetryDelay);
            }
#else
            std::error_code ec;
            if (!replace && fs::exists(to, ec))
            {
                error = "already exists";
                return false;
            }
            fs::rename(from, to, ec);
            if (ec)
                error = ec.message();
            return !ec;
#endif
        }

        bool IsPartialReleaseDir(const fs::directory_entry &entry)
        {
            const std::string name = PathUtf8(entry.path().filename());
            std::error_code ec;
            return entry.is_directory(ec) && name.size() == 16 + 8 && name.ends_with(".partial") &&
                   ParseTerrainIdHex(std::string_view(name).substr(0, 16));
        }

        std::optional<TerrainCandidateManifest> ReadManifest(const fs::path &candidate)
        {
            std::error_code ec;
            if (!fs::is_regular_file(candidate / kCandidateManifestFileName, ec))
                return std::nullopt;
            const std::optional<std::string> text = ReadText(candidate / kCandidateManifestFileName);
            if (!text)
                return std::nullopt;
            return ParseCandidateManifest(*text).manifest;
        }

        void FlipMiddleByte(const fs::path &file)
        {
            std::optional<std::vector<std::byte>> bytes = ReadBytes(file);
            if (!bytes || bytes->empty())
                return;
            (*bytes)[bytes->size() / 2] ^= std::byte{0x40};
            WriteBytes(file, *bytes);
        }

        struct Faults
        {
            const std::optional<TerrainPublishFault> &fault;

            bool At(TerrainPublishStep step) const { return fault && fault->step == step; }
            bool Interrupt(TerrainPublishStep step) const { return At(step) && fault->interrupt; }
        };

        // asset is set as soon as candidate.json names one.
        bool PublishSteps(const TerrainPublishRequest &request, const TerrainRecipe *recipe,
                          TerrainPublishLock *heldLock, TerrainReport &report, std::optional<AssetId> &asset)
        {
            const Faults faults{request.fault};
            const fs::path &candidate = request.candidateDir;
            const fs::path &root = request.releasesRoot;

            report.tiles.clear();
            ValidateTerrainCandidate(candidate, {recipe, true}, report);
            if (faults.At(TerrainPublishStep::Candidate))
                Fail(report, check::kCandidate, "", "test fault after the candidate check");
            const std::optional<TerrainCandidateManifest> manifest = ReadManifest(candidate);
            if (manifest && !manifest->release.asset.IsNull())
                asset = manifest->release.asset;
            if (!report.Ok() || !manifest || !asset)
            {
                if (report.Ok())
                    Fail(report, check::kCandidate, std::string(kCandidateManifestFileName),
                         "candidate.json changed after it validated");
                SkipNotRun(report, kPublishChecks, "the candidate did not validate; the release tree was not touched");
                return false;
            }
            const fs::path assetDir = TerrainAssetReleasesDir(root, *asset);

            std::optional<TerrainPublishLock> ownLock;
            std::string error;
            if (heldLock)
            {
                if (fs::absolute(heldLock->AssetDir()).lexically_normal() != fs::absolute(assetDir).lexically_normal())
                {
                    Fail(report, check::kLock, "", std::format("the held lock is for {}, not {}",
                                                               PathUtf8(heldLock->AssetDir()),
                                                               PathUtf8(assetDir)));
                    SkipNotRun(report, kPublishChecks, "no lock");
                    return false;
                }
            }
            else
            {
                ownLock = TerrainPublishLock::Acquire(assetDir, request.lockWait, error);
                if (!ownLock)
                {
                    Fail(report, check::kLock, std::string(kTerrainPublishLockFileName), error);
                    SkipNotRun(report, kPublishChecks, "another publisher holds this asset");
                    return false;
                }
                report.Ran(check::kLock, PathUtf8(assetDir / kTerrainPublishLockFileName));
            }
            report.publish->previous = PeekTerrainCurrent(root, *asset);

            TerrainReleaseInstall install;
            install.sourceDir = candidate;
            install.files = manifest->files;
            install.release = manifest->release;
            install.releasesRoot = root;
            install.recordedBy = std::string(kCandidateManifestFileName);
            install.fault = request.fault;
            return InstallTerrainRelease(install, recipe, report);
        }
    }

    bool InstallTerrainRelease(const TerrainReleaseInstall &install, const TerrainRecipe *recipe,
                               TerrainReport &report)
    {
        const Faults faults{install.fault};
        const fs::path &root = install.releasesRoot;
        const AssetId asset = install.release.asset;
        const TerrainReleaseId release = install.release.release;
        const std::string releaseHex = TerrainIdHex(release.value);
        const fs::path assetDir = TerrainAssetReleasesDir(root, asset);
        const fs::path finalDir = TerrainReleaseDir(root, asset, release);
        const fs::path partial = assetDir / (releaseHex + ".partial");
        std::string error;

        std::error_code ec;
        std::string replaced;
        if (fs::exists(finalDir, ec))
        {
            if (!install.replaceExisting)
            {
                Fail(report, check::kTarget, releaseHex,
                     std::format("{} already exists; a published release is never replaced or changed (bake a new "
                                 "candidate: every bake without --nonce mints a new release id)",
                                 PathUtf8(finalDir)));
                SkipNotRun(report, kInstallChecks, "nothing to publish into");
                return false;
            }
            fs::remove_all(finalDir, ec);
            if (ec)
            {
                Fail(report, check::kTarget, releaseHex,
                     std::format("cannot remove the old copy {} ({})", PathUtf8(finalDir), ec.message()));
                SkipNotRun(report, kInstallChecks, "nothing to copy into");
                return false;
            }
            replaced = " (an old copy was removed)";
        }
        // Under the lock, every .partial is left from a dead writer.
        size_t stale = 0;
        for (const fs::directory_entry &entry : fs::directory_iterator(assetDir, ec))
            if (IsPartialReleaseDir(entry))
            {
                std::error_code removeEc;
                fs::remove_all(entry.path(), removeEc);
                stale += removeEc ? 0 : 1;
            }
        fs::remove(assetDir / "current.tmp", ec);
        report.Ran(check::kTarget, std::format("{} is free{}; removed {} stale .partial directories",
                                               PathUtf8(finalDir), replaced, stale));

        if (!fs::create_directory(partial, ec))
        {
            Fail(report, check::kCopy, "", std::format("cannot create {} ({})", PathUtf8(partial),
                                                       ec ? ec.message() : "it exists"));
            SkipNotRun(report, kInstallChecks, "nothing was copied");
            return false;
        }
        auto abandon = [&](const std::string &why) {
            std::error_code ignored;
            fs::remove_all(partial, ignored);
            SkipNotRun(report, kInstallChecks, why);
            return false;
        };
        uint64_t copied = 0;
        for (size_t i = 0; i < install.files.size(); ++i)
        {
            const TerrainFileSummary &file = install.files[i];
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(install.sourceDir / file.name);
            if (!bytes || bytes->size() != file.bytes || Fnv1a64(*bytes) != file.checksum)
            {
                Fail(report, check::kCopy, file.name,
                     std::format("{} is not the {} bytes with Fnv1a64 {} that {} records (changed after it "
                                 "validated?)",
                                 PathUtf8(install.sourceDir / file.name), file.bytes,
                                 TerrainIdHex(file.checksum), install.recordedBy));
                return abandon("the copy was abandoned and its .partial directory removed");
            }
            if (i == 1 && faults.At(TerrainPublishStep::Copy))
            {
                const std::span<const std::byte> half = std::span(*bytes).first(bytes->size() / 2);
                WriteBytes(partial / file.name, half);
                Fail(report, check::kCopy, file.name,
                     std::format("test fault: the copy stopped after {} of {} bytes", half.size(), bytes->size()));
                if (faults.Interrupt(TerrainPublishStep::Copy))
                    return false;
                return abandon("the copy was abandoned and its .partial directory removed");
            }
            if (!WriteBytes(partial / file.name, *bytes) || !FlushToDisk(partial / file.name))
            {
                Fail(report, check::kCopy, file.name, std::format("cannot write {} ({})",
                                                                  PathUtf8(partial / file.name),
                                                                  LastErrorMessage()));
                return abandon("the copy was abandoned and its .partial directory removed");
            }
            copied += bytes->size();
        }
        report.Ran(check::kCopy, std::format("{} files, {} bytes, into {}", install.files.size(), copied,
                                             PathUtf8(partial)));

        if (faults.Interrupt(TerrainPublishStep::Verify))
        {
            Fail(report, check::kVerify, "", "test fault: stopped before verifying the copy");
            return false;
        }
        if (faults.At(TerrainPublishStep::Verify) && install.files.size() > 1)
            FlipMiddleByte(partial / install.files[1].name);
        // The copy has no candidate.json.
        TerrainReport copy("verify", {});
        ValidateTerrainCandidate(partial, {recipe, false}, copy);
        for (const TerrainDiagnostic &d : copy.diagnostics)
            report.Add({std::string(check::kVerify), d.field, d.tiles, d.sample,
                        std::format("[{}] {}", d.check, d.message)});
        const bool sameFiles = std::equal(
            copy.files.begin(), copy.files.end(), install.files.begin(), install.files.end(),
            [](const TerrainFileSummary &a, const TerrainFileSummary &b) {
                return a.name == b.name && a.bytes == b.bytes && a.checksum == b.checksum;
            });
        if (!sameFiles)
            Fail(report, check::kVerify, "files",
                 std::format("the copied files' sizes or checksums differ from {}", install.recordedBy));
        if (!copy.release || !(*copy.release == install.release))
            Fail(report, check::kVerify, "release.tindex", "the copied index names another asset or release");
        if (report.Status(check::kVerify) == CheckStatus::Failed)
            return abandon("the copy did not verify; its .partial directory was removed");
        report.Ran(check::kVerify, std::format("{} tiles read back; {} files match {}", copy.tiles.size(),
                                               copy.files.size(), install.recordedBy));

        if (faults.At(TerrainPublishStep::Rename))
        {
            Fail(report, check::kRename, "", "test fault before the rename");
            if (faults.Interrupt(TerrainPublishStep::Rename))
                return false;
            return abandon("the .partial directory was removed");
        }
        if (!Move(partial, finalDir, false, error))
        {
            Fail(report, check::kRename, releaseHex, std::format("cannot rename {} to {} ({})",
                                                                 PathUtf8(partial),
                                                                 PathUtf8(finalDir), error));
            return abandon("the .partial directory was removed");
        }
        report.Ran(check::kRename, PathUtf8(finalDir));

        const fs::path pointer = TerrainCurrentPointerPath(root, asset);
        const fs::path pending = assetDir / "current.tmp";
        const std::string previousText =
            report.publish->previous ? TerrainIdHex(report.publish->previous->value) : std::string("none");
        auto pointerFailed = [&](std::string why) {
            Fail(report, check::kPointer, std::string(kTerrainCurrentPointerFileName),
                 std::format("{}; release {} is complete at {} but not current, and current still names {}", why,
                             releaseHex, PathUtf8(finalDir), previousText));
            return false;
        };
        if (!WriteText(pending, EncodeTerrainCurrentPointer(release)) || !FlushToDisk(pending))
            return pointerFailed(std::format("cannot write {}", PathUtf8(pending)));
        if (faults.At(TerrainPublishStep::Pointer))
        {
            if (faults.Interrupt(TerrainPublishStep::Pointer))
            {
                Fail(report, check::kPointer, "", "test fault: stopped before replacing current");
                return false;
            }
            fs::remove(pending, ec);
            return pointerFailed("test fault before replacing current");
        }
        if (!Move(pending, pointer, true, error))
        {
            fs::remove(pending, ec);
            return pointerFailed(std::format("cannot replace {} ({})", PathUtf8(pointer), error));
        }

        const std::optional<TerrainReleaseId> readBack = PeekTerrainCurrent(root, asset);
        const TerrainResult<TerrainReleaseHandle> opened = OpenCurrentTerrainRelease(root, asset);
        if (readBack != release || !opened || opened.value->Info().release != release)
        {
            Fail(report, check::kPointer, std::string(kTerrainCurrentPointerFileName),
                 std::format("current was replaced but does not open release {}{}", releaseHex,
                             opened ? "" : ": " + opened.error.message));
            return false;
        }
        report.publish->published = true;
        report.Ran(check::kPointer, std::format("current: {} -> {}", previousText, releaseHex));
        return true;
    }

    TerrainPublishLock::TerrainPublishLock(fs::path assetDir, intptr_t handle)
        : mAssetDir(std::move(assetDir)), mHandle(handle)
    {
    }

    TerrainPublishLock::TerrainPublishLock(TerrainPublishLock &&other) noexcept
        : mAssetDir(std::move(other.mAssetDir)), mHandle(std::exchange(other.mHandle, -1))
    {
    }

    TerrainPublishLock &TerrainPublishLock::operator=(TerrainPublishLock &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            mAssetDir = std::move(other.mAssetDir);
            mHandle = std::exchange(other.mHandle, -1);
        }
        return *this;
    }

    TerrainPublishLock::~TerrainPublishLock()
    {
        Release();
    }

    void TerrainPublishLock::Release()
    {
        if (mHandle == -1)
            return;
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(mHandle));
#else
        close(static_cast<int>(mHandle));
#endif
        mHandle = -1;
    }

    std::optional<TerrainPublishLock> TerrainPublishLock::Acquire(const fs::path &assetDir,
                                                                  std::chrono::milliseconds wait, std::string &error)
    {
        std::error_code ec;
        fs::create_directories(assetDir, ec);
        if (ec)
        {
            error = std::format("cannot create {} ({})", PathUtf8(assetDir), ec.message());
            return std::nullopt;
        }
        const fs::path path = assetDir / kTerrainPublishLockFileName;
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;)
        {
#ifdef _WIN32
            // Deleted with the last handle.
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file != INVALID_HANDLE_VALUE)
                return TerrainPublishLock(assetDir, reinterpret_cast<intptr_t>(file));
            const DWORD code = GetLastError();
            // ACCESS_DENIED also covers a previous holder still closing it.
            const bool busy = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED;
            const std::string reason = std::system_category().message(static_cast<int>(code));
#else
            const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0)
                return TerrainPublishLock(assetDir, fd);
            const bool busy = fd >= 0 && errno == EWOULDBLOCK;
            const std::string reason = std::strerror(errno);
            if (fd >= 0)
                close(fd);
#endif
            if (!busy)
            {
                error = std::format("cannot open {} ({})", PathUtf8(path), reason);
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                error = std::format("another publish of this asset holds {} (waited {} ms)", PathUtf8(path),
                                    wait.count());
                return std::nullopt;
            }
            std::this_thread::sleep_for(50ms);
        }
    }

    std::optional<TerrainReleaseId> PeekTerrainCurrent(const fs::path &releasesRoot, AssetId asset,
                                                       std::string *problem)
    {
        const fs::path path = TerrainCurrentPointerPath(releasesRoot, asset);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
        {
            if (problem)
                *problem = std::format("no current release ({} does not exist)", PathUtf8(path));
            return std::nullopt;
        }
        const std::optional<std::string> text = ReadText(path);
        if (!text)
        {
            if (problem)
                *problem = std::format("cannot read {}", PathUtf8(path));
            return std::nullopt;
        }
        const TerrainResult<TerrainReleaseId> id = ParseTerrainCurrentPointer(*text);
        if (!id)
        {
            if (problem)
                *problem = std::format("{}: {}", PathUtf8(path), id.error.message);
            return std::nullopt;
        }
        return *id.value;
    }

    bool IsSameOrInside(const fs::path &a, const fs::path &b)
    {
        const fs::path rel = fs::absolute(a).lexically_normal().lexically_relative(fs::absolute(b).lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    }

    bool PublishTerrainCandidate(const TerrainPublishRequest &request, const TerrainRecipe *recipe,
                                 TerrainPublishLock *heldLock, TerrainReport &report)
    {
        if (!report.publish)
            report.publish = TerrainPublishSummary{request.releasesRoot};
        std::optional<AssetId> asset;
        const bool published = PublishSteps(request, recipe, heldLock, report, asset);
        if (asset)
            report.publish->current = PeekTerrainCurrent(request.releasesRoot, *asset);
        return published && report.Ok();
    }

    TerrainReport RunTerrainPublish(const TerrainPublishRequest &request)
    {
        TerrainReport report("publish", {check::kArguments, check::kRecipe, check::kCandidate, check::kFormat,
                                         check::kIndex, check::kPacks, check::kCoverage, check::kPayloads,
                                         check::kFinite, check::kEdges, check::kCorners, check::kRegions,
                                         check::kIdentity, check::kLock, check::kTarget, check::kCopy, check::kVerify,
                                         check::kRename, check::kPointer});
        report.candidateDir = request.candidateDir;
        report.recipePath = request.recipePath;
        report.publish = TerrainPublishSummary{request.releasesRoot};

        if (request.candidateDir.empty())
            Fail(report, check::kArguments, "--candidate", "no candidate directory");
        if (request.releasesRoot.empty())
            Fail(report, check::kArguments, "--releases", "no releases root");
        if (!request.candidateDir.empty() && !request.releasesRoot.empty() &&
            (IsSameOrInside(request.candidateDir, request.releasesRoot) ||
             IsSameOrInside(request.releasesRoot, request.candidateDir)))
            Fail(report, check::kArguments, "--candidate",
                 "the candidate and the releases root must not contain each other");
        if (report.Status(check::kArguments) == CheckStatus::Failed)
            return report;
        report.Ran(check::kArguments, std::format("candidate {}, releases {}", PathUtf8(request.candidateDir),
                                                  PathUtf8(request.releasesRoot)));

        std::optional<TerrainRecipe> recipe;
        if (request.recipePath.empty())
            report.Skip(check::kRecipe, "no --recipe given");
        else
        {
            RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
            for (RecipeIssue &issue : parsed.issues)
                report.Add({std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt,
                            std::move(issue.message)});
            if (!parsed.recipe)
                return report;
            recipe = std::move(parsed.recipe);
            report.recipeFingerprint = FingerprintRecipe(*recipe);
            report.Ran(check::kRecipe, "fingerprint " + TerrainIdHex(report.recipeFingerprint->value));
        }

        PublishTerrainCandidate(request, recipe ? &*recipe : nullptr, nullptr, report);
        return report;
    }
}
