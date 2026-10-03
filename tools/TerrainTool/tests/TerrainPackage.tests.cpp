/**
 * @file TerrainPackage.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainPackage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainPackage.h>
#include <TerrainPublish.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <fstream>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    AssetId ExampleAsset()
    {
        return MakeAssetId(ExampleAssetPath());
    }

    // A new release id each time.
    TerrainReleaseId PublishToGame(const fs::path &scratch, const fs::path &gameDir, std::string_view tag)
    {
        const fs::path candidate = scratch / "build" / std::string(tag);
        const TerrainReleaseId id = BakeCandidate(candidate);
        TerrainPublishRequest request;
        request.candidateDir = candidate;
        request.releasesRoot = TerrainReleasesRoot(gameDir);
        const TerrainReport report = RunTerrainPublish(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(report.Ok());
        return id;
    }

    TerrainReport Package(const fs::path &gameDir, const fs::path &outDir,
                          std::optional<TerrainPublishFault> fault = std::nullopt,
                          std::string_view sourcePath = kSourcePath)
    {
        TerrainPackageRequest request;
        request.idRoot = std::string(kIdRoot);
        request.sourcePath = std::string(sourcePath);
        request.gameDir = gameDir;
        request.outDir = outDir;
        request.lockWait = std::chrono::milliseconds(0);
        request.fault = fault;
        TerrainReport report = RunTerrainPackage(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        return report;
    }

    // Without the lock file, which exists only while held on Windows.
    std::vector<std::string> AssetDirNames(const fs::path &assetDir)
    {
        std::vector<std::string> names = FileNames(assetDir);
        std::erase(names, std::string(kTerrainPublishLockFileName));
        return names;
    }

    std::vector<std::string> CurrentAnd(TerrainReleaseId release)
    {
        std::vector<std::string> names{TerrainIdHex(release.value), std::string(kTerrainCurrentPointerFileName)};
        std::sort(names.begin(), names.end());
        return names;
    }

    std::string DetailOf(const TerrainReport &report, std::string_view name)
    {
        for (const TerrainCheck &c : report.checks)
            if (c.name == name)
                return c.detail;
        return {};
    }
}

TEST_CASE("Package copies only the current release, then does nothing while it matches", "[terraintool][package]")
{
    ScratchDir scratch("package_basic");
    const fs::path gameDir = scratch.path / "games" / "G";
    const fs::path outDir = scratch.path / "build" / "games" / "G";
    const fs::path outRoot = TerrainReleasesRoot(outDir);
    const AssetId asset = ExampleAsset();
    PublishToGame(scratch.path, gameDir, "old");
    const TerrainReleaseId a = PublishToGame(scratch.path, gameDir, "a");
    const auto gameTree = Snapshot(TerrainReleasesRoot(gameDir));

    const TerrainReport first = Package(gameDir, outDir);
    REQUIRE(first.Ok());
    for (std::string_view name : {check::kArguments, check::kCurrent, check::kLock, check::kIdentity, check::kTarget,
                                  check::kCopy, check::kVerify, check::kRename, check::kPointer, check::kPrune})
    {
        INFO(name << ": " << DetailOf(first, name));
        CHECK(first.Status(name) == CheckStatus::Passed);
    }
    REQUIRE(first.publish);
    CHECK(first.publish->published);
    CHECK_FALSE(first.publish->previous);
    CHECK(first.publish->current == a);
    CHECK(first.publish->releasesRoot == outRoot);

    CHECK(AssetDirNames(TerrainAssetReleasesDir(outRoot, asset)) == CurrentAnd(a));
    CHECK(Snapshot(TerrainReleaseDir(outRoot, asset, a)) == Snapshot(TerrainReleaseDir(TerrainReleasesRoot(gameDir), asset, a)));
    const TerrainResult<TerrainReleaseHandle> shipped = OpenCurrentTerrainRelease(outRoot, asset);
    const TerrainResult<TerrainReleaseHandle> published = OpenCurrentTerrainRelease(TerrainReleasesRoot(gameDir), asset);
    REQUIRE(shipped);
    REQUIRE(published);
    CHECK(shipped.value->Info() == published.value->Info());
    CHECK(ReadAllTiles(*shipped.value) == ReadAllTiles(*published.value));
    CHECK(Snapshot(TerrainReleasesRoot(gameDir)) == gameTree);

    const auto outTree = Snapshot(outRoot);
    const TerrainReport again = Package(gameDir, outDir);
    REQUIRE(again.Ok());
    CHECK(again.Status(check::kCurrent) == CheckStatus::Passed);
    CHECK(again.Status(check::kCopy) == CheckStatus::Skipped);
    CHECK_FALSE(again.publish->published);
    CHECK(again.publish->previous == a);
    CHECK(again.publish->current == a);
    CHECK(again.release->release == a);
    CHECK(Snapshot(outRoot) == outTree);
    CHECK(Snapshot(TerrainReleasesRoot(gameDir)) == gameTree);
}

TEST_CASE("Package follows the game's current and clears the rest of its output", "[terraintool][package]")
{
    ScratchDir scratch("package_follow");
    const fs::path gameDir = scratch.path / "games" / "G";
    const fs::path outDir = scratch.path / "build" / "games" / "G";
    const fs::path outRoot = TerrainReleasesRoot(outDir);
    const AssetId asset = ExampleAsset();
    const fs::path assetDir = TerrainAssetReleasesDir(outRoot, asset);

    const TerrainReleaseId a = PublishToGame(scratch.path, gameDir, "a");
    REQUIRE(Package(gameDir, outDir).Ok());
    REQUIRE(WriteText(assetDir / "notes.txt", "left by hand"));

    const TerrainReleaseId b = PublishToGame(scratch.path, gameDir, "b");
    REQUIRE(b != a);
    const fs::path outA = TerrainReleaseDir(outRoot, asset, a);
#ifdef _WIN32
    // A reader holding A's file does not fail the package.
    {
        std::ifstream reader(outA / kTerrainIndexFileName, std::ios::binary);
        REQUIRE(reader);
        const TerrainReport held = Package(gameDir, outDir);
        REQUIRE(held.Ok());
        CHECK(held.publish->previous == a);
        CHECK(held.publish->current == b);
        CHECK(fs::exists(outA / kTerrainIndexFileName));
        CHECK(DetailOf(held, check::kPrune).find("still there") != std::string::npos);
    }
    const TerrainReport later = Package(gameDir, outDir);
    REQUIRE(later.Ok());
    CHECK_FALSE(later.publish->published);
#else
    const TerrainReport later = Package(gameDir, outDir);
    REQUIRE(later.Ok());
    CHECK(later.publish->published);
#endif
    CHECK(later.publish->current == b);
    CHECK(AssetDirNames(assetDir) == CurrentAnd(b));
    CHECK(Snapshot(TerrainReleaseDir(outRoot, asset, b)) == Snapshot(TerrainReleaseDir(TerrainReleasesRoot(gameDir), asset, b)));
    CHECK(ReleaseDirNames(TerrainAssetReleasesDir(TerrainReleasesRoot(gameDir), asset)).size() == 2);
}

TEST_CASE("A failed or interrupted package keeps the output's current release readable",
          "[terraintool][package][fault]")
{
    const auto [step, interrupt] = GENERATE(table<TerrainPublishStep, bool>({
        {TerrainPublishStep::Candidate, false},
        {TerrainPublishStep::Copy, true},
        {TerrainPublishStep::Verify, false},
        {TerrainPublishStep::Rename, true},
        {TerrainPublishStep::Pointer, false},
        {TerrainPublishStep::Pointer, true},
    }));
    INFO("step " << static_cast<int>(step) << (interrupt ? ", interrupted" : ", failed"));

    ScratchDir scratch("package_fault");
    const fs::path gameDir = scratch.path / "games" / "G";
    const fs::path outDir = scratch.path / "build" / "games" / "G";
    const fs::path outRoot = TerrainReleasesRoot(outDir);
    const AssetId asset = ExampleAsset();

    const TerrainReleaseId a = PublishToGame(scratch.path, gameDir, "a");
    REQUIRE(Package(gameDir, outDir).Ok());
    const fs::path outA = TerrainReleaseDir(outRoot, asset, a);
    const auto aFiles = Snapshot(outA);

    const TerrainReleaseId b = PublishToGame(scratch.path, gameDir, "b");
    const TerrainReport faulted = Package(gameDir, outDir, TerrainPublishFault{step, interrupt});
    CHECK_FALSE(faulted.Ok());
    CHECK_FALSE(faulted.publish->published);
    CHECK(faulted.publish->current == a);
    CHECK(faulted.Status(check::kPrune) == CheckStatus::Skipped);
    CHECK(PeekTerrainCurrent(outRoot, asset) == a);
    CHECK(Snapshot(outA) == aFiles);
    const TerrainResult<TerrainReleaseHandle> reopened = OpenCurrentTerrainRelease(outRoot, asset);
    REQUIRE(reopened);
    CHECK(reopened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);

    const TerrainReport next = Package(gameDir, outDir);
    REQUIRE(next.Ok());
    CHECK(next.publish->current == b);
    CHECK(AssetDirNames(TerrainAssetReleasesDir(outRoot, asset)) == CurrentAnd(b));
}

TEST_CASE("Package refuses what it cannot ship and repairs a damaged copy of its own", "[terraintool][package]")
{
    ScratchDir scratch("package_refuse");
    const fs::path gameDir = scratch.path / "games" / "G";
    const fs::path outDir = scratch.path / "build" / "games" / "G";
    const fs::path outRoot = TerrainReleasesRoot(outDir);
    const AssetId asset = ExampleAsset();

    SECTION("nothing published")
    {
        const TerrainReport report = Package(gameDir, outDir);
        CHECK(report.Status(check::kCurrent) == CheckStatus::Failed);
        CHECK(report.Status(check::kLock) == CheckStatus::Skipped);
        CHECK_FALSE(fs::exists(outRoot));
    }
    SECTION("a corrupt pack in the game's release")
    {
        const TerrainReleaseId a = PublishToGame(scratch.path, gameDir, "a");
        const fs::path gameA = TerrainReleaseDir(TerrainReleasesRoot(gameDir), asset, a);
        const TileLocation where = Locate(gameA, {0, 0});
        std::vector<std::byte> pack = ReadFile(gameA / where.pack);
        pack[size_t(where.offset + where.length / 2)] ^= std::byte{0x10};
        WriteFile(gameA / where.pack, pack);

        const TerrainReport report = Package(gameDir, outDir);
        CHECK(report.Status(check::kPacks) == CheckStatus::Failed);
        CHECK(report.Status(check::kCopy) == CheckStatus::Skipped);
        CHECK_FALSE(PeekTerrainCurrent(outRoot, asset));
        CHECK(ReleaseDirNames(TerrainAssetReleasesDir(outRoot, asset)).empty());
    }
    SECTION("an output inside the game's releases, or the game itself")
    {
        PublishToGame(scratch.path, gameDir, "a");
        const auto gameTree = Snapshot(TerrainReleasesRoot(gameDir));
        CHECK(Package(gameDir, gameDir).Status(check::kArguments) == CheckStatus::Failed);
        CHECK(Package(gameDir, TerrainReleasesRoot(gameDir) / "x").Status(check::kArguments) == CheckStatus::Failed);
        CHECK(Package(gameDir, outDir, std::nullopt, "terrain/relief.json").Status(check::kArguments) ==
              CheckStatus::Failed);
        CHECK(Snapshot(TerrainReleasesRoot(gameDir)) == gameTree);
        CHECK_FALSE(fs::exists(outRoot));
    }
    SECTION("a truncated pack in the output's copy")
    {
        const TerrainReleaseId a = PublishToGame(scratch.path, gameDir, "a");
        REQUIRE(Package(gameDir, outDir).Ok());
        const fs::path outA = TerrainReleaseDir(outRoot, asset, a);
        const TileLocation where = Locate(outA, {0, 0});
        std::vector<std::byte> pack = ReadFile(outA / where.pack);
        pack.resize(pack.size() / 2);
        WriteFile(outA / where.pack, pack);

        const TerrainReport report = Package(gameDir, outDir);
        REQUIRE(report.Ok());
        CHECK(report.publish->published);
        CHECK(report.publish->previous == a);
        CHECK(report.publish->current == a);
        CHECK(DetailOf(report, check::kTarget).find("old copy") != std::string::npos);
        CHECK(Snapshot(outA) == Snapshot(TerrainReleaseDir(TerrainReleasesRoot(gameDir), asset, a)));
    }
}
