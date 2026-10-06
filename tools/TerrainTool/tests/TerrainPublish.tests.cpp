/**
 * @file TerrainPublish.tests.cpp
 * @author Jackey Cen
 * @brief Tests for TerrainTool/TerrainPublish.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <TerrainPublish.h>

#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <thread>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;
using namespace std::chrono_literals;

namespace
{
    TerrainReport Publish(const fs::path &candidate, const fs::path &releases,
                          std::optional<TerrainPublishFault> fault = std::nullopt,
                          std::chrono::milliseconds lockWait = 0ms)
    {
        TerrainPublishRequest request;
        request.candidateDir = candidate;
        request.releasesRoot = releases;
        request.fault = fault;
        request.lockWait = lockWait;
        TerrainReport report = RunTerrainPublish(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        return report;
    }

    AssetId ExampleAsset()
    {
        return MakeAssetId(ExampleAssetPath());
    }

    // Without the lock file, which exists only while held on Windows.
    std::vector<std::string> AssetDirNames(const fs::path &assetDir)
    {
        std::vector<std::string> names = FileNames(assetDir);
        std::erase(names, std::string(kTerrainPublishLockFileName));
        return names;
    }

    std::string_view CheckOf(TerrainPublishStep step)
    {
        switch (step)
        {
        case TerrainPublishStep::Candidate:
            return check::kCandidate;
        case TerrainPublishStep::Copy:
            return check::kCopy;
        case TerrainPublishStep::Verify:
            return check::kVerify;
        case TerrainPublishStep::Rename:
            return check::kRename;
        case TerrainPublishStep::Pointer:
            return check::kPointer;
        }
        return "";
    }
}

TEST_CASE("Publish copies only the release files and moves current last", "[terraintool][publish]")
{
    ScratchDir scratch("publish_basic");
    const fs::path candidate = scratch.path / "build" / "candidate";
    const fs::path releases = scratch.path / "games" / "G" / "terrain" / "releases";
    const TerrainReleaseId id = BakeCandidate(candidate);
    const auto candidateBefore = Snapshot(candidate);

    const TerrainReport report = Publish(candidate, releases);
    REQUIRE(report.Ok());
    for (const TerrainCheck &c : report.checks)
    {
        INFO(c.name << ": " << c.detail);
        CHECK(c.status == (c.name == check::kRecipe ? CheckStatus::Skipped : CheckStatus::Passed));
    }
    REQUIRE(report.publish);
    CHECK(report.publish->published);
    CHECK_FALSE(report.publish->previous);
    CHECK(report.publish->current == id);
    CHECK(report.release->release == id);
    REQUIRE(report.tiles.size() == 24);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(), [](const auto &t) { return t.source; }) == 4);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(), [](const auto &t) { return t.kind == 1; }) == 8);
    CHECK(std::count_if(report.tiles.begin(), report.tiles.end(), [](const auto &t) { return t.kind == 2; }) == 8);

    const fs::path assetDir = TerrainAssetReleasesDir(releases, ExampleAsset());
    std::vector<std::string> names{TerrainIdHex(id.value), "current"};
    std::sort(names.begin(), names.end());
    CHECK(AssetDirNames(assetDir) == names);
    const fs::path releaseDir = TerrainReleaseDir(releases, ExampleAsset(), id);
    auto expected = candidateBefore;
    expected.erase(std::string(kCandidateManifestFileName));
    CHECK(Snapshot(releaseDir) == expected);
    CHECK(Snapshot(candidate) == candidateBefore);
    CHECK(ReadText(TerrainCurrentPointerPath(releases, ExampleAsset())) == TerrainIdHex(id.value));

    const TerrainResult<TerrainReleaseHandle> current = OpenCurrentTerrainRelease(releases, ExampleAsset());
    REQUIRE(current);
    const TerrainResult<TerrainReleaseHandle> baked = TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(candidate));
    REQUIRE(baked);
    CHECK(current.value->Info() == baked.value->Info());
    CHECK(ReadAllTiles(*current.value) == ReadAllTiles(*baked.value));
}

TEST_CASE("A fault at any publish step leaves the previous current release complete and readable",
          "[terraintool][publish][fault]")
{
    const auto [step, interrupt] = GENERATE(table<TerrainPublishStep, bool>({
        {TerrainPublishStep::Candidate, false},
        {TerrainPublishStep::Candidate, true},
        {TerrainPublishStep::Copy, false},
        {TerrainPublishStep::Copy, true},
        {TerrainPublishStep::Verify, false},
        {TerrainPublishStep::Verify, true},
        {TerrainPublishStep::Rename, false},
        {TerrainPublishStep::Rename, true},
        {TerrainPublishStep::Pointer, false},
        {TerrainPublishStep::Pointer, true},
    }));
    INFO("step " << static_cast<int>(step) << (interrupt ? ", interrupted" : ", failed"));

    ScratchDir scratch("publish_fault");
    const fs::path releases = scratch.path / "releases";
    const AssetId asset = ExampleAsset();
    const fs::path assetDir = TerrainAssetReleasesDir(releases, asset);

    const TerrainReleaseId a = BakeCandidate(scratch.path / "a");
    REQUIRE(Publish(scratch.path / "a", releases).Ok());
    const fs::path aDir = TerrainReleaseDir(releases, asset, a);
    const auto aFiles = Snapshot(aDir);
    const TerrainResult<TerrainReleaseHandle> openA = OpenCurrentTerrainRelease(releases, asset);
    REQUIRE(openA);
    const std::vector<TileHeights> aTiles = ReadAllTiles(*openA.value);

    const TerrainReleaseId b = BakeCandidate(scratch.path / "b");
    REQUIRE(b != a);
    const TerrainReport faulted = Publish(scratch.path / "b", releases, TerrainPublishFault{step, interrupt});
    CHECK_FALSE(faulted.Ok());
    CHECK(faulted.Status(CheckOf(step)) == CheckStatus::Failed);
    REQUIRE(faulted.publish);
    CHECK_FALSE(faulted.publish->published);
    CHECK(faulted.publish->current == a);

    // Readers still find A, whole.
    CHECK(PeekTerrainCurrent(releases, asset) == a);
    CHECK(Snapshot(aDir) == aFiles);
    const TerrainResult<TerrainReleaseHandle> reopened = OpenCurrentTerrainRelease(releases, asset);
    REQUIRE(reopened);
    CHECK(reopened.value->Info().release == a);
    CHECK(reopened.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
    CHECK(ReadAllTiles(*reopened.value) == aTiles);
    CHECK(ReadAllTiles(*openA.value) == aTiles);

    const fs::path bDir = TerrainReleaseDir(releases, asset, b);
    const fs::path bPartial = assetDir / (TerrainIdHex(b.value) + ".partial");
    if (step == TerrainPublishStep::Pointer)
    {
        const TerrainResult<TerrainReleaseHandle> orphan = OpenTerrainRelease(releases, asset, b);
        REQUIRE(orphan);
        CHECK(orphan.value->VerifyAllPacks().code == TerrainPackErrorCode::None);
    }
    else
        CHECK_FALSE(fs::exists(bDir));
    const bool leftPartial = interrupt && (step == TerrainPublishStep::Copy || step == TerrainPublishStep::Verify ||
                                           step == TerrainPublishStep::Rename);
    CHECK(fs::exists(bPartial) == leftPartial);
    CHECK(fs::exists(assetDir / "current.tmp") == (interrupt && step == TerrainPublishStep::Pointer));

    const TerrainReleaseId c = BakeCandidate(scratch.path / "c");
    const TerrainReport next = Publish(scratch.path / "c", releases);
    REQUIRE(next.Ok());
    CHECK(next.publish->previous == a);
    CHECK(PeekTerrainCurrent(releases, asset) == c);
    CHECK_FALSE(fs::exists(bPartial));
    CHECK_FALSE(fs::exists(assetDir / "current.tmp"));
    CHECK(Snapshot(aDir) == aFiles);
    CHECK(fs::exists(bDir) == (step == TerrainPublishStep::Pointer));
}

TEST_CASE("Publish A, open A, publish B: both stay readable and A never follows current", "[terraintool][publish]")
{
    ScratchDir scratch("publish_ab");
    const fs::path releases = scratch.path / "releases";
    const AssetId asset = ExampleAsset();

    const TerrainReleaseId a = BakeCandidate(scratch.path / "a");
    REQUIRE(Publish(scratch.path / "a", releases).Ok());
    const TerrainResult<TerrainReleaseHandle> handleA = OpenCurrentTerrainRelease(releases, asset);
    REQUIRE(handleA);
    const TerrainReleaseInfo infoA = handleA.value->Info();
    const std::vector<TileHeights> tilesA = ReadAllTiles(*handleA.value);
    CHECK(infoA.release == a);

    const fs::path recipeB = WriteSeedRecipe(scratch.path / "recipes", 7);
    const TerrainReleaseId b = BakeCandidate(scratch.path / "b", std::nullopt, recipeB);
    REQUIRE(Publish(scratch.path / "b", releases).Ok());
    CHECK(PeekTerrainCurrent(releases, asset) == b);

    CHECK(handleA.value->Info() == infoA);
    CHECK(ReadAllTiles(*handleA.value) == tilesA);

    const TerrainResult<TerrainReleaseHandle> handleB = OpenCurrentTerrainRelease(releases, asset);
    REQUIRE(handleB);
    const TerrainReleaseInfo infoB = handleB.value->Info();
    const std::vector<TileHeights> tilesB = ReadAllTiles(*handleB.value);
    CHECK(infoB.release == b);
    CHECK(infoB.asset == infoA.asset);
    CHECK(infoB.source != infoA.source);
    CHECK(tilesB != tilesA);

    const TerrainResult<TerrainReleaseHandle> byIdA = OpenTerrainRelease(releases, asset, a);
    REQUIRE(byIdA);
    CHECK(byIdA.value->Info() == infoA);
    CHECK(ReadAllTiles(*byIdA.value) == tilesA);

    fs::remove(TerrainCurrentPointerPath(releases, asset));
    CHECK_FALSE(PeekTerrainCurrent(releases, asset));
    CHECK(ReadAllTiles(*handleA.value) == tilesA);
    CHECK(ReadAllTiles(*handleB.value) == tilesB);
}

TEST_CASE("Publish refuses a damaged candidate before touching the release tree", "[terraintool][publish]")
{
    ScratchDir scratch("publish_damaged");
    const fs::path candidate = scratch.path / "candidate";
    const fs::path releases = scratch.path / "releases";
    BakeCandidate(candidate);

    SECTION("corrupt pack")
    {
        const TileLocation where = Locate(candidate, {0, 0});
        std::vector<std::byte> pack = ReadFile(candidate / where.pack);
        pack[size_t(where.offset + where.length / 2)] ^= std::byte{0x10};
        WriteFile(candidate / where.pack, pack);
        const TerrainReport report = Publish(candidate, releases);
        CHECK(report.Status(check::kPacks) == CheckStatus::Failed);
    }
    SECTION("no candidate.json")
    {
        fs::remove(candidate / kCandidateManifestFileName);
        const TerrainReport report = Publish(candidate, releases);
        CHECK(report.Status(check::kCandidate) == CheckStatus::Failed);
    }
    SECTION("stray file")
    {
        REQUIRE(WriteText(candidate / "notes.txt", "not a release file"));
        const TerrainReport report = Publish(candidate, releases);
        CHECK(report.Status(check::kCandidate) == CheckStatus::Failed);
    }
    CHECK_FALSE(fs::exists(releases));
}

TEST_CASE("Publish never replaces a published release", "[terraintool][publish]")
{
    ScratchDir scratch("publish_twice");
    const fs::path releases = scratch.path / "releases";
    const TerrainReleaseId first = BakeCandidate(scratch.path / "one", kNonce);
    REQUIRE(Publish(scratch.path / "one", releases).Ok());
    const auto tree = Snapshot(releases);

    REQUIRE(BakeCandidate(scratch.path / "two", kNonce) == first);
    const TerrainReport again = Publish(scratch.path / "two", releases);
    CHECK_FALSE(again.Ok());
    CHECK(again.Status(check::kTarget) == CheckStatus::Failed);
    CHECK(again.Status(check::kCopy) == CheckStatus::Skipped);
    CHECK_FALSE(again.publish->published);
    CHECK(again.publish->current == first);
    CHECK(Snapshot(releases) == tree);
}

TEST_CASE("A second publisher of one asset waits for the lock or fails", "[terraintool][publish][lock]")
{
    ScratchDir scratch("publish_lock");
    const fs::path candidate = scratch.path / "candidate";
    const fs::path releases = scratch.path / "releases";
    const TerrainReleaseId id = BakeCandidate(candidate);
    const fs::path assetDir = TerrainAssetReleasesDir(releases, ExampleAsset());

    std::string error;
    std::optional<TerrainPublishLock> held = TerrainPublishLock::Acquire(assetDir, 0ms, error);
    REQUIRE(held);
    CHECK_FALSE(TerrainPublishLock::Acquire(assetDir, 100ms, error));
    CHECK(error.find("another publish") != std::string::npos);
    CHECK(TerrainPublishLock::Acquire(TerrainAssetReleasesDir(releases, AssetId{1}), 0ms, error));

    const TerrainReport busy = Publish(candidate, releases);
    CHECK(busy.Status(check::kLock) == CheckStatus::Failed);
    CHECK(busy.Status(check::kCopy) == CheckStatus::Skipped);
    CHECK(ReleaseDirNames(assetDir).empty());
    CHECK_FALSE(PeekTerrainCurrent(releases, ExampleAsset()));

    std::thread holder([&] {
        std::this_thread::sleep_for(300ms);
        held.reset();
    });
    const TerrainReport waited = Publish(candidate, releases, std::nullopt, 10s);
    holder.join();
    REQUIRE(waited.Ok());
    CHECK(PeekTerrainCurrent(releases, ExampleAsset()) == id);
}
