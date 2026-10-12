/**
 * @file TerrainStreamingSettings.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainStreamingSettings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainStreamingSettings.h>

#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr std::string_view kComplete = R"({
        "cpu": { "budgetMiB": 64 },
        "io": { "workerThreads": 2, "maxInstallsPerFrame": 16 },
        "retry": { "maxAttempts": 3, "firstDelaySeconds": 0.05, "maxDelaySeconds": 0.8, "failedHoldSeconds": 4 },
        "residency": { "mode": "Auto", "queryLeaseSeconds": 2.5 },
        "view": { "refineErrorPixels": 1.5, "mergeErrorPixels": 1.0, "prefetchErrorPixels": 0.5,
                  "prefetchBudgetMiB": 8 },
        "gpu": { "budgetMiB": 256, "uploadMiBPerFrame": 12, "stagingMiB": 24, "meshRetainSeconds": 3 }
    })";

    struct Files
    {
        std::filesystem::path dir = MakeScratchDir("streaming_settings");
        std::filesystem::path game = dir / "game" / kTerrainStreamingSettingsFileName;
        std::filesystem::path local = dir / "exe" / kTerrainStreamingLocalSettingsFileName;

        Files()
        {
            std::filesystem::create_directories(game.parent_path());
            std::filesystem::create_directories(local.parent_path());
        }
        ~Files() { std::filesystem::remove_all(dir); }

        TerrainStreamingSettingsLoad Load(std::string_view gameText, std::string_view localText = {})
        {
            REQUIRE(WriteText(game, std::string(gameText)));
            if (!localText.empty())
                REQUIRE(WriteText(local, std::string(localText)));
            return LoadTerrainStreamingSettings(game, local);
        }
    };

    bool Reports(const TerrainStreamingSettingsLoad &load, std::string_view file, std::string_view text)
    {
        return std::any_of(load.problems.begin(), load.problems.end(),
                           [&](const std::string &problem)
                           {
                               UNSCOPED_INFO(problem);
                               return problem.starts_with(file) && problem.find(text) != std::string::npos;
                           });
    }
}

TEST_CASE("A complete settings file loads and the machine-local file overrides it key by key",
          "[terrain][streaming][settings]")
{
    Files files;
    const TerrainStreamingSettingsLoad game = files.Load(kComplete);
    REQUIRE(game.problems.empty());
    REQUIRE(game.settings);
    CHECK(game.gameFileFound);
    CHECK_FALSE(game.localFileFound);
    const TerrainStreamingSettings &s = *game.settings;
    CHECK(s.cpu.budgetBytes == uint64_t(64) * 1024 * 1024);
    CHECK(s.io.workerThreads == 2);
    CHECK(s.io.maxInstallsPerFrame == 16);
    CHECK(s.retry.maxAttempts == 3);
    CHECK(s.retry.firstDelaySeconds == 0.05);
    CHECK(s.retry.maxDelaySeconds == 0.8);
    CHECK(s.retry.failedHoldSeconds == 4.0);
    CHECK(s.residency.mode == TerrainResidencyMode::Auto);
    CHECK(s.residency.queryLeaseSeconds == 2.5);
    CHECK(s.view.refineErrorPixels == 1.5);
    CHECK(s.view.mergeErrorPixels == 1.0);
    CHECK(s.view.prefetchErrorPixels == 0.5);
    CHECK(s.view.prefetchBudgetBytes == uint64_t(8) * 1024 * 1024);
    CHECK(s.gpu.budgetBytes == uint64_t(256) * 1024 * 1024);
    CHECK(s.gpu.uploadBytesPerFrame == uint64_t(12) * 1024 * 1024);
    CHECK(s.gpu.stagingBytes == uint64_t(24) * 1024 * 1024);
    CHECK(s.gpu.meshRetainSeconds == 3.0);
    CHECK(ValidateTerrainStreamingSettings(s).empty());

    const TerrainStreamingSettingsLoad merged =
        files.Load(kComplete, R"({ "cpu": { "budgetMiB": 1536 }, "io": { "workerThreads": 1 },
                                   "view": { "prefetchBudgetMiB": 0 }, "gpu": { "uploadMiBPerFrame": 0.5 } })");
    REQUIRE(merged.settings);
    CHECK(merged.localFileFound);
    CHECK(merged.settings->cpu.budgetBytes == uint64_t(1536) * 1024 * 1024);
    CHECK(merged.settings->io.workerThreads == 1);
    CHECK(merged.settings->io.maxInstallsPerFrame == 16);
    CHECK(merged.settings->residency.mode == TerrainResidencyMode::Auto);
    CHECK(merged.settings->view.prefetchBudgetBytes == uint64_t(0));
    CHECK(merged.settings->view.refineErrorPixels == 1.5);
    CHECK(merged.settings->gpu.uploadBytesPerFrame == uint64_t(512) * 1024);
    CHECK(merged.settings->gpu.budgetBytes == uint64_t(256) * 1024 * 1024);
}

TEST_CASE("Missing, unknown and invalid settings are reported by path and file, and nothing loads",
          "[terrain][streaming][settings]")
{
    Files files;
    const std::string game = files.game.generic_string();
    const std::string local = files.local.generic_string();

    SECTION("No game file")
    {
        const TerrainStreamingSettingsLoad load = LoadTerrainStreamingSettings(files.game, files.local);
        CHECK_FALSE(load.settings);
        CHECK_FALSE(load.gameFileFound);
        CHECK(Reports(load, game, "missing"));
    }
    SECTION("Every missing field, by path")
    {
        const TerrainStreamingSettingsLoad load =
            files.Load(R"({ "cpu": {}, "io": { "workerThreads": 2 }, "retry": { "maxAttempts": 2 } })");
        CHECK_FALSE(load.settings);
        for (const char *path : {"cpu.budgetMiB is missing", "io.maxInstallsPerFrame is missing",
                                 "retry.firstDelaySeconds is missing", "retry.maxDelaySeconds is missing",
                                 "retry.failedHoldSeconds is missing", "residency is missing", "view is missing",
                                 "gpu is missing"})
            CHECK(Reports(load, game, path));
        CHECK(load.problems.size() == 8);
    }
    SECTION("Unknown keys and wrong types")
    {
        const TerrainStreamingSettingsLoad load = files.Load(R"({
            "cpu": { "budgetMiB": "lots", "budgetMib": 64 },
            "io": { "workerThreads": -1, "maxInstallsPerFrame": 1.5 },
            "retry": { "maxAttempts": 3, "firstDelaySeconds": 0.05, "maxDelaySeconds": 0.8, "failedHoldSeconds": 4 },
            "residency": { "mode": "Lazy", "queryLeaseSeconds": 2.5 },
            "view": { "refineErrorPixels": 1.5, "mergeErrorPixels": 1.0, "prefetchErrorPixels": 0.5,
                      "prefetchBudgetMiB": "some" },
            "gpu": { "budgetMiB": 256, "uploadMiBPerFrame": 12, "stagingMiB": 24, "meshRetainSeconds": 3 },
            "upload": {}
        })");
        CHECK_FALSE(load.settings);
        CHECK(Reports(load, game, "cpu.budgetMiB must be a finite number"));
        CHECK(Reports(load, game, "cpu.budgetMib is not a known setting"));
        CHECK(Reports(load, game, "io.workerThreads must be a whole number"));
        CHECK(Reports(load, game, "io.maxInstallsPerFrame must be a whole number"));
        CHECK(Reports(load, game, "residency.mode must be \"Auto\", \"Eager\" or \"Interest\""));
        CHECK(Reports(load, game, "view.prefetchBudgetMiB must be a finite number"));
        CHECK(Reports(load, game, "upload is not a known section"));
        CHECK(load.problems.size() == 7);
    }
    SECTION("Invalid values name the file that set them")
    {
        const TerrainStreamingSettingsLoad load = files.Load(
            kComplete, R"({ "cpu": { "budgetMiB": 0 }, "io": { "workerThreads": 65 }, "retry": { "maxDelaySeconds": 0.01 },
                            "view": { "mergeErrorPixels": 2, "prefetchBudgetMiB": -1 },
                            "gpu": { "uploadMiBPerFrame": 0, "meshRetainSeconds": -1 } })");
        CHECK_FALSE(load.settings);
        CHECK(Reports(load, local, "cpu.budgetMiB must be positive"));
        CHECK(Reports(load, local, "io.workerThreads must be 1 to 64"));
        CHECK(Reports(load, local, "retry.maxDelaySeconds must be a finite number of seconds, at least firstDelaySeconds"));
        CHECK(Reports(load, local, "view.mergeErrorPixels must be a finite positive number of pixels, at most refineErrorPixels"));
        CHECK(Reports(load, local, "view.prefetchBudgetMiB must be zero or more"));
        CHECK(Reports(load, local, "gpu.uploadMiBPerFrame must be positive"));
        CHECK(Reports(load, local, "gpu.meshRetainSeconds must be a finite number of seconds, zero or more"));
        CHECK(load.problems.size() == 7);
    }
    SECTION("A file that is not a JSON object")
    {
        CHECK(Reports(files.Load("[1, 2]"), game, "not a JSON object"));
        CHECK(Reports(files.Load(kComplete, "7"), local, "not a JSON object"));
    }
}

TEST_CASE("Validation names every field of settings built in code that was left unset", "[terrain][streaming][settings]")
{
    const std::vector<std::string> unset = ValidateTerrainStreamingSettings({});
    CHECK(unset.size() == 17);
    for (const char *path : {"cpu.budgetMiB", "io.workerThreads", "io.maxInstallsPerFrame", "retry.maxAttempts",
                             "retry.firstDelaySeconds", "retry.maxDelaySeconds", "retry.failedHoldSeconds",
                             "residency.mode", "residency.queryLeaseSeconds", "view.refineErrorPixels",
                             "view.mergeErrorPixels", "view.prefetchErrorPixels", "view.prefetchBudgetMiB",
                             "gpu.budgetMiB", "gpu.uploadMiBPerFrame", "gpu.stagingMiB", "gpu.meshRetainSeconds"})
        CHECK(std::any_of(unset.begin(), unset.end(), [&](const std::string &p) { return p.starts_with(path); }));

    TerrainStreamingSettings s;
    s.cpu.budgetBytes = 1;
    s.io.workerThreads = 1;
    s.io.maxInstallsPerFrame = 1;
    s.retry.maxAttempts = 1;
    s.retry.firstDelaySeconds = 0.0;
    s.retry.maxDelaySeconds = 0.0;
    s.retry.failedHoldSeconds = 0.0;
    s.residency.mode = TerrainResidencyMode::Interest;
    s.residency.queryLeaseSeconds = 0.25;
    s.view.refineErrorPixels = 2.0;
    s.view.mergeErrorPixels = 2.0;
    s.view.prefetchErrorPixels = 2.0;
    s.view.prefetchBudgetBytes = 0;
    s.gpu.budgetBytes = 1;
    s.gpu.uploadBytesPerFrame = 1;
    s.gpu.stagingBytes = 1;
    s.gpu.meshRetainSeconds = 0.0;
    CHECK(ValidateTerrainStreamingSettings(s).empty());
    s.residency.queryLeaseSeconds = 0.0;
    CHECK(ValidateTerrainStreamingSettings(s) == std::vector<std::string>{
                                                     "residency.queryLeaseSeconds: must be a finite positive number of seconds"});
    s.residency.queryLeaseSeconds = 0.25;
    s.view.prefetchErrorPixels = 2.5;
    CHECK(ValidateTerrainStreamingSettings(s) ==
          std::vector<std::string>{
              "view.prefetchErrorPixels: must be a finite positive number of pixels, at most mergeErrorPixels"});
}
