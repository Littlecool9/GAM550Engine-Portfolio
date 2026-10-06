/**
 * @file TerrainSync.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainSync.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainPublish.h>
#include <TerrainSync.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <thread>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using namespace std::chrono_literals;

namespace
{
    // games/G, a build directory and a recipe.
    struct SyncTree
    {
        ScratchDir scratch;
        fs::path gameDir = scratch.path / "games" / "G";
        fs::path releases = TerrainReleasesRoot(gameDir);
        fs::path recipe = gameDir / "assets" / "terrain" / "relief-2x2.terrain.json";
        AssetId asset = MakeAssetId(ExampleAssetPath());
        fs::path assetDir = TerrainAssetReleasesDir(releases, asset);

        explicit SyncTree(std::string_view tag) : scratch(tag)
        {
            fs::create_directories(recipe.parent_path());
            fs::copy_file(ExampleRecipePath(), recipe);
        }

        TerrainSyncRequest Request(std::string_view build = "build") const
        {
            TerrainSyncRequest request;
            request.recipePath = recipe;
            request.idRoot = std::string(kIdRoot);
            request.sourcePath = std::string(kSourcePath);
            request.gameDir = gameDir;
            request.candidateDir = scratch.path / build / "terrain" / "G" / "relief-2x2";
            return request;
        }

        void SetSeed(uint64_t seed) const
        {
            WriteSeedRecipe(recipe.parent_path(), seed, recipe.filename().string());
        }
    };

    TerrainReport Sync(const TerrainSyncRequest &request)
    {
        TerrainReport report = RunTerrainSync(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        for (const TerrainCheck &c : report.checks)
            UNSCOPED_INFO(c.name << ": " << CheckStatusName(c.status) << " " << c.detail);
        return report;
    }
}

TEST_CASE("Sync publishes once, then does nothing while the recipe is unchanged", "[terraintool][sync]")
{
    SyncTree tree("sync_noop");
    const TerrainReport first = Sync(tree.Request());
    REQUIRE(first.Ok());
    REQUIRE(first.publish);
    CHECK(first.publish->published);
    CHECK_FALSE(first.publish->previous);
    REQUIRE(first.publish->current);
    CHECK(first.publish->releasesRoot == tree.releases);
    CHECK(first.Status(check::kPointer) == CheckStatus::Passed);
    const TerrainReleaseId published = *first.publish->current;
    CHECK(ReleaseDirNames(tree.assetDir) == std::vector<std::string>{TerrainIdHex(published.value)});
    const auto before = Snapshot(tree.releases);

    for (int run = 0; run < 2; ++run)
    {
        INFO("repeat " << run);
        const TerrainReport again = Sync(tree.Request());
        REQUIRE(again.Ok());
        CHECK_FALSE(again.publish->published);
        CHECK(again.publish->previous == published);
        CHECK(again.publish->current == published);
        REQUIRE(again.release);
        CHECK(again.release->release == published);
        CHECK(again.release->asset == tree.asset);
        CHECK(again.Status(check::kCurrent) == CheckStatus::Passed);
        CHECK(again.Status(check::kGenerate) == CheckStatus::Skipped);
        CHECK(again.Status(check::kPointer) == CheckStatus::Skipped);
        CHECK(Snapshot(tree.releases) == before);
    }

    // Another build directory (Debug and Release) finds the same release current.
    const TerrainReport otherBuild = Sync(tree.Request("build-release"));
    REQUIRE(otherBuild.Ok());
    CHECK_FALSE(otherBuild.publish->published);
    CHECK_FALSE(fs::exists(otherBuild.candidateDir));
    CHECK(Snapshot(tree.releases) == before);
}

TEST_CASE("Sync publishes a changed recipe as a new release and keeps the old one", "[terraintool][sync]")
{
    SyncTree tree("sync_changed");
    const TerrainReport first = Sync(tree.Request());
    REQUIRE(first.Ok());
    const TerrainReleaseId r1 = *first.publish->current;
    const auto r1Files = Snapshot(TerrainReleaseDir(tree.releases, tree.asset, r1));
    const TerrainResult<TerrainReleaseHandle> open1 = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(open1);
    const std::vector<TileHeights> tiles1 = ReadAllTiles(*open1.value);

    tree.SetSeed(7);
    const TerrainReport changed = Sync(tree.Request());
    REQUIRE(changed.Ok());
    CHECK(changed.publish->published);
    CHECK(changed.publish->previous == r1);
    REQUIRE(changed.publish->current);
    const TerrainReleaseId r2 = *changed.publish->current;
    CHECK(r2 != r1);
    for (const TerrainCheck &c : changed.checks)
        if (c.name == check::kCurrent)
            CHECK(c.detail.find("fingerprint") != std::string::npos);

    CHECK(ReleaseDirNames(tree.assetDir).size() == 2);
    CHECK(Snapshot(TerrainReleaseDir(tree.releases, tree.asset, r1)) == r1Files);
    CHECK(ReadAllTiles(*open1.value) == tiles1);
    const TerrainResult<TerrainReleaseHandle> open2 = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(open2);
    CHECK(open2.value->Info().release == r2);
    CHECK(ReadAllTiles(*open2.value) != tiles1);

    const TerrainReport settled = Sync(tree.Request());
    REQUIRE(settled.Ok());
    CHECK_FALSE(settled.publish->published);
    CHECK(settled.publish->current == r2);
}

TEST_CASE("A missing or damaged current release makes the next sync publish again", "[terraintool][sync]")
{
    SyncTree tree("sync_damaged");
    REQUIRE(Sync(tree.Request()).Ok());

    SECTION("releases tree deleted")
    {
        fs::remove_all(tree.releases);
        const TerrainReport report = Sync(tree.Request());
        REQUIRE(report.Ok());
        CHECK(report.publish->published);
        CHECK_FALSE(report.publish->previous);
    }
    SECTION("pack missing")
    {
        const TerrainReleaseId r1 = *PeekTerrainCurrent(tree.releases, tree.asset);
        const fs::path r1Dir = TerrainReleaseDir(tree.releases, tree.asset, r1);
        fs::remove(r1Dir / "chunk_0_0.tpack");
        const TerrainReport report = Sync(tree.Request());
        REQUIRE(report.Ok());
        CHECK(report.publish->published);
        CHECK(report.publish->previous == r1);
        CHECK(fs::exists(r1Dir / kTerrainIndexFileName));
    }
    SECTION("pointer garbage")
    {
        REQUIRE(WriteText(TerrainCurrentPointerPath(tree.releases, tree.asset), "not a release"));
        const TerrainReport report = Sync(tree.Request());
        REQUIRE(report.Ok());
        CHECK(report.publish->published);
        CHECK_FALSE(report.publish->previous);
    }
    SECTION("pointer to a missing release")
    {
        REQUIRE(WriteText(TerrainCurrentPointerPath(tree.releases, tree.asset), "00000000000000aa"));
        const TerrainReport report = Sync(tree.Request());
        REQUIRE(report.Ok());
        CHECK(report.publish->published);
    }
    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(tree.releases, tree.asset);
    REQUIRE(current);
    CHECK(current.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
}

TEST_CASE("Sync deletes only its own candidate and never one inside the releases tree", "[terraintool][sync]")
{
    SyncTree tree("sync_refuse");
    TerrainSyncRequest request = tree.Request();

    SECTION("a directory that is not a candidate")
    {
        fs::create_directories(request.candidateDir);
        REQUIRE(WriteText(request.candidateDir / "notes.txt", "mine"));
        const TerrainReport report = Sync(request);
        CHECK(report.Status(check::kWrite) == CheckStatus::Failed);
        CHECK(ReadText(request.candidateDir / "notes.txt") == "mine");
    }
    SECTION("release files without candidate.json, as in a published release")
    {
        fs::create_directories(request.candidateDir);
        REQUIRE(WriteText(request.candidateDir / kTerrainIndexFileName, "index"));
        REQUIRE(WriteText(request.candidateDir / "chunk_0_0.tpack", "pack"));
        const TerrainReport report = Sync(request);
        CHECK(report.Status(check::kWrite) == CheckStatus::Failed);
        CHECK(ReadText(request.candidateDir / kTerrainIndexFileName) == "index");
        CHECK(ReadText(request.candidateDir / "chunk_0_0.tpack") == "pack");
    }
    SECTION("a candidate inside the releases tree")
    {
        request.candidateDir = tree.releases / "candidate";
        const TerrainReport report = Sync(request);
        CHECK(DiagnosticsOf(report, check::kArguments).size() == 1);
    }
    CHECK_FALSE(PeekTerrainCurrent(tree.releases, tree.asset));
    CHECK((!fs::exists(tree.assetDir) || ReleaseDirNames(tree.assetDir).empty()));
}

TEST_CASE("Sync uses the shared path rule: slashes agree, case does not", "[terraintool][sync][identity]")
{
    SyncTree tree("sync_spelling");
    TerrainSyncRequest request = tree.Request();
    request.sourcePath = "terrain\\relief-2x2.terrain.json";
    const TerrainReport backslash = Sync(request);
    REQUIRE(backslash.Ok());
    CHECK(backslash.assetPath == ExampleAssetPath());
    const TerrainReleaseId published = *backslash.publish->current;

    const TerrainReport slash = Sync(tree.Request());
    REQUIRE(slash.Ok());
    CHECK_FALSE(slash.publish->published);
    CHECK(slash.publish->current == published);

    // A case that differs from the file on disk would be another asset.
    request.sourcePath = "terrain/Relief-2x2.terrain.json";
    const TerrainReport cased = Sync(request);
    const std::vector<TerrainDiagnostic> args = DiagnosticsOf(cased, check::kArguments);
    REQUIRE(args.size() == 1);
    CHECK(args[0].field == "--source-path");
    CHECK(args[0].message.find("keep case") != std::string::npos);
    const AssetId other = ResolveTerrainAssetPath(kIdRoot, request.sourcePath).asset;
    CHECK(other != tree.asset);
    CHECK_FALSE(fs::exists(TerrainAssetReleasesDir(tree.releases, other)));

    request.sourcePath = "../relief-2x2.terrain.json";
    CHECK(DiagnosticsOf(Sync(request), check::kArguments).size() == 1);
    CHECK(ReleaseDirNames(tree.assetDir) == std::vector<std::string>{TerrainIdHex(published.value)});
}

TEST_CASE("Two builds syncing one asset at once publish it once", "[terraintool][sync][lock]")
{
    SyncTree tree("sync_race");
    std::array<std::optional<TerrainReport>, 2> reports;
    std::thread debug([&] { reports[0] = RunTerrainSync(tree.Request("build-debug")); });
    std::thread release([&] { reports[1] = RunTerrainSync(tree.Request("build-release")); });
    debug.join();
    release.join();

    REQUIRE(reports[0]);
    REQUIRE(reports[1]);
    for (const std::optional<TerrainReport> &r : reports)
        for (const TerrainDiagnostic &d : r->diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
    REQUIRE(reports[0]->Ok());
    REQUIRE(reports[1]->Ok());
    CHECK(reports[0]->publish->published != reports[1]->publish->published);
    CHECK(reports[0]->publish->current == reports[1]->publish->current);
    CHECK(ReleaseDirNames(tree.assetDir).size() == 1);

    std::string error;
    std::optional<TerrainPublishLock> held = TerrainPublishLock::Acquire(tree.assetDir, 0ms, error);
    REQUIRE(held);
    TerrainSyncRequest impatient = tree.Request();
    impatient.lockWait = 0ms;
    CHECK(Sync(impatient).Status(check::kLock) == CheckStatus::Failed);
}
